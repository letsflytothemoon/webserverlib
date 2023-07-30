#pragma once
#include <iostream>
#include <thread>
#include <mutex>
#include <queue>
#include <boost/beast.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ip = asio::ip;
using tcp = ip::tcp;

namespace webserverlibhelpers
{
    template <class CharT>
    struct CharTypeSpecific
    {
        static constexpr std::ostream &out = std::cout;
        static constexpr std::ostream &err = std::cerr;
    };

    template <>
    struct CharTypeSpecific<wchar_t>
    {
        static constexpr std::wostream &out = std::wcout;
        static constexpr std::wostream &err = std::wcerr;
    };

    template <class Id>
    struct Prop
    {
        typename Id::ValueType _value;
        Prop() {}
        Prop(Prop& other) : _value(other._value) { }
        Prop(const Prop& other) : _value(other._value) { }
        Prop(typename Id::ValueType value) : _value(value) { }
        operator typename Id::ValueType() const { return _value; }
    };

    template <class ValueT>
    struct ClassId
    { typedef ValueT ValueType; };

    struct AddressId      : ClassId<std::string   > { };
    struct PortId         : ClassId<unsigned short> { };
    struct ThreadsCountId : ClassId<unsigned int  > { };

    typedef Prop<AddressId     > Address;
    typedef Prop<PortId        > Port;
    typedef Prop<ThreadsCountId> ThreadsCount;

    template <class Needed, class NotMatched, class ... Nexts>
    struct Selector
    {
        static const Needed& Result(const NotMatched&, const Nexts& ... nexts)
        { return Selector<Needed, Nexts ...>::Result(nexts ...); }
    };

    template <class Needed, class ... Nexts>
    struct Selector<Needed, Needed, Nexts ...>
    {
        static const Needed& Result(const Needed& needed, const Nexts& ...)
        { return needed; }
    };

    template <class Needed, class ... Args>
    static const Needed& Select(Args&& ... args)
    { return Selector<Needed, Args ...>::Result(args ...); }
}

template <class CharT>
struct webserverlib
{
    typedef std::basic_string<CharT> StringT;

    using BodyT = http::basic_dynamic_body<beast::basic_multi_buffer<std::allocator<CharT>>>;
    using Request = http::request<BodyT>;
    using Response = http::response<BodyT>;

    typedef webserverlibhelpers::Prop<webserverlibhelpers::AddressId     > Address;
    typedef webserverlibhelpers::Prop<webserverlibhelpers::PortId        > Port;
    typedef webserverlibhelpers::Prop<webserverlibhelpers::ThreadsCountId> Threads;

    class WebServer
    {
    protected:
        template <class ... Types>
        using Selector = webserverlibhelpers::Selector<Types ...>;

        typedef webserverlibhelpers::CharTypeSpecific<CharT> CharTypeSpecific;
        static constexpr std::basic_ostream<CharT> &out = CharTypeSpecific::out;
        static constexpr std::basic_ostream<CharT> &err = CharTypeSpecific::err;

        std::string    _address;
        unsigned short _port;
        unsigned int   _threads;

        std::queue<tcp::socket> _acceptedConnectionsQueue;
        std::mutex _connectionsQueueMutex;

        void ListenLoop()
        {
            try
            {
                ip::address address = ip::make_address(_address);
                asio::io_context io_context(1);

                tcp::acceptor acceptor{io_context, {address, _port}};
                acceptor.non_blocking(true);

                while (true)
                {
                    tcp::socket socket{io_context};
                    boost::system::error_code error_code;
                    
                    while (acceptor.accept(socket, error_code) == asio::error::would_block)
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));

                    {
                        std::lock_guard<std::mutex> lock_guard(_connectionsQueueMutex);
                        _acceptedConnectionsQueue.push(std::move(socket));
                    }
                }
            }
            catch (const std::exception &exception)
            {
                std::cerr << exception.what();
            }
        }

        bool AcceptedConnectionsQueueEmpty()
        {
            std::lock_guard<std::mutex> lock_guard(_connectionsQueueMutex);
            return _acceptedConnectionsQueue.empty();
        }

        tcp::socket PopAcceptedConnection()
        {
            while (true)
            {
                while (AcceptedConnectionsQueueEmpty())
                    std::this_thread::sleep_for(std::chrono::microseconds(5));
                
                std::lock_guard<std::mutex> lock_guard(_connectionsQueueMutex);
                if (!_acceptedConnectionsQueue.empty())
                {
                    struct Poper
                    {
                        std::queue<tcp::socket> &_queue;
                        Poper(std::queue<tcp::socket> &queue) : _queue(queue) {}
                        ~Poper() { _queue.pop(); }
                    } poper(_acceptedConnectionsQueue);
                    return (std::move(_acceptedConnectionsQueue.front()));
                }
            }
        }

        void RequestProcessorLoop()
        {
            while (true)
            {
                try
                {
                    tcp::socket socket = PopAcceptedConnection();
                    Request request;
                    beast::flat_buffer buffer(8192);

                    http::read(
                        socket,
                        buffer,
                        request);

                    Response response;
                    response.version(request.version());

                    beast::ostream(response.body()) << ".!.";

                    http::write(
                        socket,
                        response);

                    socket.shutdown(tcp::socket::shutdown_send);
                }
                catch (const std::exception exception)
                {
                    std::cerr << "processing request error: " << exception.what() << std::endl;
                }
            }
        }

    public:
        template <class ... Args>
        WebServer(Args&& ... args) :
        _address (webserverlibhelpers::Select<Address>(args ..., Address("0.0.0.0"))),
        _port    (webserverlibhelpers::Select<Port   >(args ..., Port          (80))),
        _threads (webserverlibhelpers::Select<Threads>(args ..., Threads        (1)))
        {
            std::cout << "address : " << _address << std::endl;
            std::cout << "port    : " << _port    << std::endl;
            std::cout << "threads : " << _threads << std::endl;
            
            std::thread(&WebServer::ListenLoop, this).detach();
            for (unsigned int i = 0; i < _threads; i++)
                std::thread(&WebServer::RequestProcessorLoop, this).detach();
        }
    };
};