#include <statefs/property.hpp>
#include <cor/util.hpp>
#include <cor/udev.hpp>
#include <cor/udev/util.hpp>
#include <cor/error.hpp>

#include <thread>
#include <set>
#include <memory>
#include <array>
#include <functional>
#include <time.h>

#include <boost/asio.hpp>
#include <boost/asio/posix/basic_descriptor.hpp>
#include <boost/asio/basic_stream_socket.hpp>

#define TRACE() std::cerr << "statefs-kbd: "
namespace cor {

}

namespace asio = boost::asio;
namespace udevpp = cor::udevpp;
using cor::str;

namespace statefs {

namespace udev {

template <typename FnT>
void for_each_device(udevpp::Root &root, FnT const &fn, char const *subsystem)
{
    udevpp::Enumerate e(root);
    e.subsystem_add(subsystem);
    auto devs = e.devices();
    devs.for_each([&root, &fn](udevpp::DeviceInfo const &info) {
            fn(udevpp::Device{root, info.path()});
        });
}


class Monitor
{
public:
    enum class Action { Poll, Stop };
    typedef std::function<Action (udevpp::Device &&)> callback_type;

    Monitor(asio::io_service &
            , udevpp::Root &
            , char const *
            , callback_type);
    void run();
private:
    asio::io_service &io_;
    udevpp::Root &root_;
    udevpp::Monitor mon_;
    asio::posix::stream_descriptor stream_;
    callback_type on_device_;
    std::function<void()> async_read_;
};

enum class KeyboardProp {
    Present, Open, EOE // end of enum
        };

class KeyboardNs : public BasicNamespace<KeyboardProp>
{
public:
    KeyboardNs(asio::io_service &io, udevpp::Root &root);
    virtual void release() { }

    Monitor::Action on_input_device(udevpp::Device &&dev);

    template <typename T>
    T get_on_input_device()
    {
        return std::bind(&KeyboardNs::on_input_device, this, std::placeholders::_1);
    }

private:
    asio::io_service &io_;
    udevpp::Root &root_;
    std::unique_ptr<Monitor> mon_;
    std::set<std::string> keyboards_;
};

Monitor::Monitor(asio::io_service &io
                 , udevpp::Root &root
                 , char const *subsystem
                 , callback_type on_device)
    : io_(io)
    , root_(root)
    , mon_([&root, subsystem]() {
            if (!root)
                throw cor::Error("Root is not initialized");
            return udevpp::Monitor(root, subsystem, nullptr);
        }())
    , stream_(io, [this]() {
            auto fd = mon_.fd();
            if (fd < 0)
                throw cor::Error("Monitor fd is invalid");
            return fd;
        }())
    , on_device_(on_device)
{}

void Monitor::run()
{
    TRACE() << "Starting input monitoring\n";
    using boost::system::error_code;
    auto on_event = [this](error_code ec, std::size_t) {
        if (ec) {
            TRACE() << "asio err " << ec << std::endl;
            io_.stop();
            return;
        }
        if (on_device_(mon_.device(root_)) == Action::Poll)
            async_read_();
    };
    using namespace std::placeholders;
    auto event_wrapper = std::bind
        (cor::error_trace_nothrow
         <decltype(on_event), error_code, std::size_t>
         , on_event, _1, _2);
    async_read_ = [this, event_wrapper]() {
        stream_.async_read_some(asio::null_buffers(), event_wrapper);
    };
    async_read_();
}

KeyboardNs::KeyboardNs(asio::io_service &io, udevpp::Root &root)
    : BasicNamespace<KeyboardProp>("maemo_InternalKeyboard")
    , io_(io)
    , root_(root)
{
    using namespace std::placeholders;
    auto fn = std::bind(&KeyboardNs::on_input_device, this, _1);
    mon_ = cor::make_unique<Monitor>(io, root, "input", fn);
    for_each_device(root_, fn, "input");
    mon_->run();
}

Monitor::Action KeyboardNs::on_input_device(udevpp::Device &&dev)
{
    auto path = dev.path();
    bool is_changed = (udevpp::is_keyboard(dev)
                       ? keyboards_.insert(path).second
                       : (keyboards_.erase(path) != 0));
    if (is_changed) {
        TRACE() << "Keyboards count: " << keyboards_.size() << std::endl;
        auto is_available = statefs_attr(keyboards_.size() != 0);
        set(KeyboardProp::Open, is_available);
        set(KeyboardProp::Present, is_available);
    }
    return Monitor::Action::Poll;
}

using std::make_tuple;

class Provider;
static Provider *provider = nullptr;

class Provider : public statefs::AProvider
{
public:
    Provider(statefs_server *server)
        : AProvider("udev-keyboard", server)
    {
        auto ns = std::make_shared<KeyboardNs>(io_, root_);
        insert(std::static_pointer_cast<statefs::ANode>(ns));
        // monitor thread is started after read operation is queued by
        // KeyboardNs
        monitor_thread_ = cor::make_unique<std::thread>
            ([this]() {
                TRACE() << "Monitor thread is started" << std::endl;
                io_.run();
                TRACE() << "Monitor thread is exiting" << std::endl;
            });
    }

    virtual ~Provider()
    {
        TRACE() << "Stopping I/O" << std::endl;
        io_.stop();
        monitor_thread_->join();
    }

    virtual void release() {
        if (this == provider) {
            delete provider;
            provider = nullptr;
        }
    }

private:
    asio::io_service io_;
    udevpp::Root root_;
    std::unique_ptr<std::thread> monitor_thread_;
};

static inline Provider *init_provider(statefs_server *server)
{
    if (provider)
        throw std::logic_error("provider ptr is already set");
    provider = new Provider(server);
    return provider;
}


}

using statefs::udev::KeyboardProp;
using std::make_tuple;

template <>
const BasicNamespace<KeyboardProp>::info_type
BasicNamespace<KeyboardProp>::property_info = {{
        make_tuple("Present", "0", PropType::Discrete)
        , make_tuple("Open", "0", PropType::Discrete)
    }};

}

EXTERN_C struct statefs_provider * statefs_provider_get
(struct statefs_server *server)
{
    return statefs::udev::init_provider(server);
}
