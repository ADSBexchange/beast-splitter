#include <algorithm>
#include <iomanip>
#include <boost/asio.hpp>

#include "beast_input.h"
#include "modes_message.h"

using namespace beast;

enum class SerialInput::ParserState { RESYNC, FIND_1A, TEST_TYPE, READ_1A, READ_TYPE, READ_DATA, READ_ESCAPED_1A };
enum class SerialInput::ReceiverType { UNKNOWN, BEAST, RADARCAPE };

SerialInput::SerialInput(boost::asio::io_service &service_,
                         const std::string &path_,
                         unsigned int fixed_baud_rate_,
                         const Settings &fixed_settings_,
                         const modes::Filter &filter_)
    : path(path_),
      port(service_),      
      reconnect_timer(service_),
      receiver_type(ReceiverType::UNKNOWN),
      fixed_settings(fixed_settings_),
      filter(filter_),
      receiving_gps_timestamps(false),
      autobaud_interval(autobaud_base_interval),
    autobaud_timer(service_),
    autodetect_timer(service_),
    good_sync(0),
    bad_sync(0),
    bytes_since_sync(0),
    readbuf(std::make_shared<helpers::bytebuf>(read_buffer_size)),
    state(ParserState::RESYNC)
{
    // set up autobaud
    if (fixed_baud_rate_ == 0) {
        autobauding = true;
        autobaud_rates.assign(autobaud_standard_rates.begin(), autobaud_standard_rates.end());
    } else {
        autobauding = false;
        autobaud_rates.push_back(fixed_baud_rate_);
    }

    baud_rate = autobaud_rates.begin();
}

void SerialInput::start(void)
{
    auto self(shared_from_this());

    std::cerr << "set baud rate " << *baud_rate << std::endl;
    try {
        if (port.is_open())
            port.cancel();
        else
            port.open(path);
        port.set_option(boost::asio::serial_port_base::character_size(8));
        port.set_option(boost::asio::serial_port_base::stop_bits(boost::asio::serial_port_base::stop_bits::one));
        port.set_option(boost::asio::serial_port_base::parity(boost::asio::serial_port_base::parity::none));
        port.set_option(boost::asio::serial_port_base::flow_control(boost::asio::serial_port_base::flow_control::hardware));
        port.set_option(boost::asio::serial_port_base::baud_rate(*baud_rate));
    } catch (const boost::system::system_error &err) {
        handle_error(err.code());
        return;
    }

    // set receiver type
    receiving_gps_timestamps = false;
    autodetect_timer.cancel();
    if (fixed_settings.radarcape.on())
        receiver_type = ReceiverType::RADARCAPE;
    else if (fixed_settings.radarcape.off())
        receiver_type = ReceiverType::BEAST;
    else {                
        receiver_type = ReceiverType::UNKNOWN;
        autodetect_timer.expires_from_now(radarcape_detect_interval);
        autodetect_timer.async_wait([this,self] (const boost::system::error_code &ec) {
                if (!ec) {
                    std::cerr << "autodetect timer fired, assuming beast receiver" << std::endl;
                    receiver_type = ReceiverType::BEAST;
                    send_settings_message();
                }
            });
    }

    send_settings_message();
    start_reading();

    if (autobaud_rates.size() > 1) {
        autobaud_timer.expires_from_now(autobaud_interval);

        autobaud_timer.async_wait([this,self] (const boost::system::error_code &ec) {
                if (!ec) {
                    std::cerr << "autobaud timer fired" << std::endl;
                    advance_autobaud();
                }
            });
    }
}

void SerialInput::send_settings_message()
{
    // apply fixed settings, let the filter set anything else that's not fixed
    Settings settings = fixed_settings | Settings(filter);

    // always use our idea of the radarcape setting, and always use binary format
    settings.radarcape = (receiver_type == ReceiverType::RADARCAPE);
    settings.binary_format = true;

    std::cerr << "sending settings message: " << settings.apply_defaults() << std::endl;

    // send it
    auto self(shared_from_this());
    auto message = std::make_shared<helpers::bytebuf>(settings.to_message());
    boost::asio::async_write(port, boost::asio::buffer(*message),
                             [this,self,message] (boost::system::error_code ec, std::size_t len) {
                                 if (ec)
                                     handle_error(ec);
                             });
}

void SerialInput::set_filter(const modes::Filter &newfilter)
{
    if (filter != newfilter) {
        filter = newfilter;
        if (port.is_open())
            send_settings_message();
    }
}

void SerialInput::handle_error(const boost::system::error_code &ec)
{
    std::cerr << "got error " << ec.message() << std::endl;
    if (ec == boost::asio::error::operation_aborted)
        return;

    autobaud_timer.cancel();
    autodetect_timer.cancel();
    if (port.is_open()) {
        boost::system::error_code ignored;
        port.close(ignored);
    }

    // reset autobaud state
    if (!autobaud_rates.empty()) {
        autobauding = true;
        autobaud_interval = autobaud_base_interval;
        baud_rate = autobaud_rates.begin();
    }

    // schedule reconnect.
    auto self(shared_from_this());
    reconnect_timer.expires_from_now(reconnect_interval);
    reconnect_timer.async_wait([this,self] (const boost::system::error_code &ec) {
            if (!ec) {
                std::cerr << "reconnect timer fired" << std::endl;
                start();
            }
        });    
}

void SerialInput::advance_autobaud(void)
{
    if (!autobauding)
        return;

    if (++baud_rate == autobaud_rates.end()) {
        // Ran out of rates to try. Increase the interval and start again
        baud_rate = autobaud_rates.begin();
        autobaud_interval = std::min(autobaud_max_interval, autobaud_interval * 2);
    }

    std::cerr << "advanced autobaud to try " << *baud_rate << std::endl;

    start();
}

void SerialInput::start_reading(void)
{
    auto self(shared_from_this());
    std::shared_ptr<helpers::bytebuf> buf;

    buf.swap(readbuf);
    if (buf) {
        buf->resize(read_buffer_size);
    } else {
        std::cerr << "start_reading had to make a new buffer" << std::endl;
        buf = std::make_shared<helpers::bytebuf>(read_buffer_size);
    }

    port.async_read_some(boost::asio::buffer(*buf),
                         [this,self,buf] (const boost::system::error_code &ec, std::size_t len) {
                             if (ec) {
                                 readbuf = buf;
                                 std::cerr << "async_read_some handler stopped with an error" << std::endl;
                                 handle_error(ec);
                             } else {
                                 buf->resize(len);
                                 parse_input(*buf);
                                 readbuf = buf;
                                 start_reading();
                             }
                         });
}

void SerialInput::lost_sync(void)
{
    if (good_sync < 5) {
        ++bad_sync;
    } else {
        bad_sync = 0;
    }

    state = ParserState::RESYNC;
    good_sync = 0;
    bytes_since_sync = 0;

    if (!autobauding && autobaud_rates.size() > 1 && bad_sync > 50) {
        std::cerr << "restart autobauding" << std::endl;
        // We picked a rate, but it's not really working, let's
        // restart autobauding.
        autobauding = true;
        advance_autobaud();
    }
}

void SerialInput::parse_input(const helpers::bytebuf &buf)
{
    /*
    std::cerr << std::hex << std::setfill('0');
    for (auto ch : buf)
        std::cerr << std::setw(2) << (int)ch << " ";
    std::cerr << std::dec << std::endl;
    */

    auto p = buf.begin();

    while (p != buf.end()) {
        switch (state) {
        case ParserState::RESYNC:
            // Scanning for <not-1A> <1A> <typebyte> <data...>
            for (; p != buf.end(); ++p) {
                if (*p != 0x1A) {
                    state = ParserState::FIND_1A;
                    break;
                }

                if (++bytes_since_sync > max_bytes_without_sync) {
                    // provoke a lost_sync() periodically
                    // while we do not have sync
                    lost_sync();
                    break;
                }

                //std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)*p << std::dec << " ";
            }
            break;

        case ParserState::FIND_1A:
            // Scanning for <1A> <typebyte> <data...>
            for (; p != buf.end(); ++p) {
                if (*p == 0x1A) {
                    //std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)*p << std::dec << " ";
                    state = ParserState::TEST_TYPE;
                    ++p;
                    break;
                }

                if (++bytes_since_sync > max_bytes_without_sync) {
                    // provoke a lost_sync() periodically
                    // while we do not have sync
                    lost_sync();
                    break;
                }

                //std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)*p << std::dec << " ";
            }
            break;

        case ParserState::READ_1A:
            // Expecting <1A> <typebyte> <data...>
            if (*p == 0x1A) {
                //std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)*p << std::dec << " ";
                state = ParserState::READ_TYPE;
                ++p;
            } else {
                std::cerr << "SYNC: READ_1A: wasn't 1A" << std::endl;
                lost_sync();
            }
            break;

        case ParserState::TEST_TYPE:
        case ParserState::READ_TYPE:
            // Expecting <typebyte> <data...>
            messagetype = messagetype_from_byte(*p);
            if (messagetype == modes::MessageType::INVALID) {
                if (state == ParserState::READ_TYPE) {                    
                    std::cerr << "SYNC: READ_TYPE: wasn't a valid type" << std::endl;
                    lost_sync();
                } else { // TEST_TYPE            
                    state = ParserState::FIND_1A;
                }
            } else {
                //std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)*p << std::dec << " ";
                metadata.clear();
                messagedata.clear();
                state = ParserState::READ_DATA;
                ++p;
            }
            break;

        case ParserState::READ_DATA:
            {
                // Reading message contents
                std::size_t msglen = modes::message_size(messagetype);
                while (p != buf.end() && messagedata.size() < msglen) {
                    uint8_t b = *p++;
                    //std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)b << std::dec << " ";
                    if (b == 0x1A) {
                        if (p == buf.end()) {
                            // Can't handle it this time around.
                            state = ParserState::READ_ESCAPED_1A;
                            break;
                        }

                        if (*p != 0x1A) {
                            std::cerr << "SYNC: READ_DATA: bad 1A escape" << std::endl;
                            lost_sync();
                            break;
                        }

                        // valid 1A escape, consume it
                        //std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)*p << std::dec << " ";
                        ++p;

                    }
                    
                    if (metadata.size() < 7)
                        metadata.push_back(b);
                    else
                        messagedata.push_back(b);
                }
                
                if (messagedata.size() >= msglen) {
                    // Done with this message.
                    dispatch_message();
                    state = ParserState::READ_1A;
                }
            }
            break;

        case ParserState::READ_ESCAPED_1A:
            {
                // This happens if we see a 1A as the final
                // byte of a read; READ_DATA cannot handle
                // the escape immediately and sets state to
                // READ_ESCAPED_1A to handle the first
                // byte of the next read.

                if (*p != 0x1A) {
                    std::cerr << "SYNC: READ_ESCAPED_1A: bad 1A escape" << std::endl;
                    lost_sync();
                    break;
                }

                // valid 1A escape
                //std::cerr << std::hex << std::setfill('0') << std::setw(2) << (int)*p << std::dec << " ";
                if (metadata.size() < 7)
                    metadata.push_back(*p++);
                else
                    messagedata.push_back(*p++);

                if (messagedata.size() >= modes::message_size(messagetype)) {
                    dispatch_message();
                    state = ParserState::READ_1A;
                } else {
                    state = ParserState::READ_DATA;
                }
            }
            break;
            
        default:
            // WAT
            lost_sync();
            break;
        }
    }
}

void SerialInput::dispatch_message()
{
    if (++good_sync >= autobaud_good_syncs_needed) {
        good_sync = autobaud_good_syncs_needed;
        bad_sync = 0;
        bytes_since_sync = 0;

        if (autobauding) {
            // We are autobauding and this rate looks good.
            std::cerr << "autobaud succeeded, rate " << *baud_rate << std::endl;
            autobauding = false;
            bad_sync = 0;
            autobaud_timer.cancel();
        }
    }

    // if we are not convinced of this rate, don't process messages yet
    if (autobauding) {
        std::cerr << "x";
        return;
    }

    // monitor status messages for GPS timestamp bit
    // and for radarcape autodetection
    if (messagetype == modes::MessageType::STATUS) {
        receiving_gps_timestamps = Settings(messagedata[0]).gps_timestamps.on();
        if (receiver_type == ReceiverType::UNKNOWN) {
            std::cerr << "detected radarcape" << std::endl;
            receiver_type = ReceiverType::RADARCAPE;
            autodetect_timer.cancel();            
            send_settings_message(); // for the g/G setting
        }
    }

    if (receiver_type == ReceiverType::UNKNOWN) {
        // still trying to autodetect, swallow messages
        return;
    }

    if (!message_notifier)
        return;

    // basic decoding, then pass it on.
    std::uint64_t timestamp =
        ((std::uint64_t)metadata[0] << 40) |
        ((std::uint64_t)metadata[1] << 32) |
        ((std::uint64_t)metadata[2] << 24) | 
        ((std::uint64_t)metadata[3] << 16) |
        ((std::uint64_t)metadata[4] << 8) |
        ((std::uint64_t)metadata[5]);

    std::uint8_t signal = metadata[6];

    // dispatch it
    message_notifier(modes::Message(messagetype,
                                    receiving_gps_timestamps ? modes::TimestampType::GPS : modes::TimestampType::TWELVEMEG,
                                    timestamp,
                                    signal,
                                    std::move(messagedata)));
    messagedata.clear(); // make sure we leave it in a valid state after moving
}
