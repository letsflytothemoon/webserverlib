#pragma once
#include <iostream>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>
#include <boost/beast.hpp>
#include <boost/algorithm/string.hpp>

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
        constexpr static const char* slash = "/";
    };

    template <>
    struct CharTypeSpecific<wchar_t>
    {
        static constexpr std::wostream &out = std::wcout;
        static constexpr std::wostream &err = std::wcerr;
        constexpr static const wchar_t* slash = L"/";
    };

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
}

template <class CharT>
struct webserverlib
{
    typedef std::basic_string<CharT> string;
    typedef std::basic_ostringstream<CharT> ostringstream;
    typedef webserverlibhelpers::CharTypeSpecific<CharT> CharTypeSpecific;
    static constexpr std::basic_ostream<CharT> &out = CharTypeSpecific::out;
    static constexpr std::basic_ostream<CharT> &err = CharTypeSpecific::err;
    using BodyT = http::basic_dynamic_body<beast::basic_multi_buffer<std::allocator<CharT>>>;
    using Request = http::request<BodyT>;
    using Response = http::response<BodyT>;

    template <class Id>
    struct Prop
    {  
        typedef typename Id::ValueType ValueType;
        ValueType _value;
        Prop() {}

        Prop(typename Id::ValueType value) : _value(value) { }

        Prop(Prop& other) : _value(other._value) { }

        Prop(const Prop& other) : _value(other._value) { }

        template <class T, class U, class ... Args>
        Prop(const T& t, const U& u, const Args& ... args) :
        _value(webserverlibhelpers::Selector<Prop<Id>, T, U, Args ...>::Result(t, u, args ...)._value)
        { }

    };
    
    class HttpRequestContext
    {
    protected:
        Request            _request;
        std::queue<string> _route;
        ostringstream      _responseStream;
    public:
        Request&            request        = _request;
        std::queue<string>& route          = _route;
        ostringstream&      responseStream = _responseStream;

        HttpRequestContext(http::request<http::basic_dynamic_body<beast::basic_multi_buffer<std::allocator<CharT>>>> request) :
        _request(std::move(request))
        {
            std::vector<std::basic_string<CharT>> path;

            boost::split(path, request.target(), boost::is_any_of(CharTypeSpecific::slash));

            for(auto i = ++path.begin(); i != path.end(); i++)
                route.push(*i);
        }

        string GetPathStep()
        {
            string result(std::move(route.front()));
            route.pop();
            return result;
        }
    };

    class WebServer
    {
    public:
        template <class ValueT>
        struct ClassId
        { typedef ValueT ValueType; };

        struct AddressId      : ClassId<std::string   > { };
        struct PortId         : ClassId<unsigned short> { };
        struct ThreadsCountId : ClassId<unsigned int  > { };
        struct RequestProcId  :
        ClassId<std::function<void(HttpRequestContext&)> > { };

        typedef Prop<AddressId     > Address;
        typedef Prop<PortId        > Port;
        typedef Prop<ThreadsCountId> Threads;
        typedef Prop<RequestProcId > RequestProc;
    protected:
        typename Address    ::ValueType _address;
        typename Port       ::ValueType _port;
        typename Threads    ::ValueType _threads;
        typename RequestProc::ValueType _requestProc;

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

                    HttpRequestContext httpRequestContext(std::move(request));
                    
                    _requestProc(httpRequestContext);

                    beast::ostream(response.body()) << httpRequestContext.responseStream.str();

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
        WebServer(const Args& ... args) :
        _address    (Address(args ..., Address("0.0.0.0"))._value),
        _port       (Port   (args ..., Port          (80))._value),
        _threads    (Threads(args ..., Threads        (1))._value),
        _requestProc(RequestProc(args ...)._value)
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