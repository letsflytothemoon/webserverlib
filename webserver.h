#pragma once
#include <iostream>
#include <fstream>
#include <thread>
#include <mutex>
#include <queue>
#include <functional>
#include <utility>
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

    template <class Id>
    struct Prop
    {  
        typedef typename Id::ValueType ValueType;
        ValueType value;
        Prop() {}

        Prop(typename Id::ValueType value) : value(value) { }

        Prop(Prop& other) : value(other.value) { }

        Prop(const Prop& other) : value(other.value) { }

        template <class T, class U, class ... Args>
        Prop(const T& t, const U& u, const Args& ... args) :
        value(webserverlibhelpers::Selector<Prop<Id>, T, U, Args ...>::Result(t, u, args ...).value)
        { }

    };
}

class NotFoundException : public std::exception
{
public:
    const char* what() const noexcept override
    { return "404 - not found"; }
};

class BadRequestException : public std::exception
{
public:
    const char* what() const noexcept override
    { return "bad request"; }
};

using Body     = http::dynamic_body;
using Request  = http::request<Body>;
using Response = http::response<Body>;

class HttpRequestContext
{
public:
    Request                 request;
    std::queue<std::string> route;
    std::stringstream       responseStream;

    HttpRequestContext(Request request) :
    request(std::move(request))
    {
        std::vector<std::string> path;

        boost::split(path, request.target(), boost::is_any_of("/"));

        for(auto i = ++path.begin(); i != path.end(); i++)
            route.push(*i);
    }

    std::string GetPathStep()
    {
        std::string result(std::move(route.front()));
        route.pop();
        return std::move(result);
    }
};

namespace routing
{
    class RouterEndPoint;

    class Router
    {
    public:
        virtual RouterEndPoint& GetEndPoint(HttpRequestContext&) = 0;
        virtual ~Router() { }
    };

    class RouterNode : public Router
    {
        std::map<std::string, Router*> routes;
    public:
        RouterNode(std::initializer_list<std::pair<const std::string, Router*>> init) :
        routes(init)
        { }

        template <class ... Args>
        RouterNode(Args&& ... args) :
        routes(args ...)
        { }

        ~RouterNode()
        {
            for(auto i = routes.begin(); i != routes.end(); i++)
                delete i->second;
        }

        RouterEndPoint& GetEndPoint(HttpRequestContext& context) override
        {
            std::string pathStep = context.GetPathStep();
            auto i = routes.find(pathStep);
            if(i == routes.end())
                throw NotFoundException();
            return i->second->GetEndPoint(context);
        }
    };

    class RouterEndPoint : public Router
    {
    public:
        RouterEndPoint& GetEndPoint(HttpRequestContext& context) override
        { return *this; }

        virtual void ProcessRequest(HttpRequestContext&) = 0;
    };

    class StaticDirectoryEndPoint : public RouterEndPoint
    {
        std::string dirName;
    public:
        StaticDirectoryEndPoint(std::string dirName) : dirName(std::move(dirName))
        { }

        void ProcessRequest(HttpRequestContext& context) override
        {
            std::stringstream pathStringStream;
            pathStringStream << dirName;

            while(!context.route.empty())
            {

                std::string pathStep = context.route.front();
                context.route.pop();
                if(pathStep == "..")
                    throw BadRequestException();
                pathStringStream << "/" << pathStep;
            }

            std::ifstream fstream(pathStringStream.str());
            context.responseStream << fstream.rdbuf();
        }
    };

    class StaticDocumentEndPoint : public RouterEndPoint
    {
        std::string fileName;
    public:
        StaticDocumentEndPoint(std::string fileName) : fileName(std::move(fileName))
        { }
        
        void ProcessRequest(HttpRequestContext& context) override
        {
            std::ifstream fstream(fileName);
            context.responseStream << fstream.rdbuf();
        }
    };

    template <class Callable, class Controller>
    class ApiEndPoint : public RouterEndPoint
    {
        Controller controller;
        Callable   callable;
    public:
        ApiEndPoint(Controller controller, Callable callable)  :
        controller(controller),
        callable  (callable)
        { }

        void SendResponse(HttpRequestContext& context) override
        { (controller->*callable)(context); }
    };
}


class WebServer
{
protected:
    template <class ValueT>
    struct ClassId
    { typedef ValueT ValueType; };

    struct AddressId      : ClassId<std::string     > { };
    struct PortId         : ClassId<unsigned short  > { };
    struct ThreadsCountId : ClassId<unsigned int    > { };
    struct RouterId       : ClassId<routing::Router*> { };
public:
    typedef webserverlibhelpers::Prop<AddressId     > Address;
    typedef webserverlibhelpers::Prop<PortId        > Port;
    typedef webserverlibhelpers::Prop<ThreadsCountId> Threads;

    class Router
    {
        routing::Router* ptrRouter;
    public:
        Router(Router&& other)
        { std::swap(ptrRouter, other.ptrRouter); }

        Router(routing::Router* ptrRouter) : ptrRouter(ptrRouter) { }

        operator routing::Router*() const
        { return ptrRouter; }
        ~Router() { delete ptrRouter; }
    };

protected:
    typename Address    ::ValueType address;
    typename Port       ::ValueType port;
    typename Threads    ::ValueType threads;
    routing::Router*                ptrRouter;

public:
    std::queue<tcp::socket> _acceptedConnectionsQueue;
    std::mutex _connectionsQueueMutex;

    void ListenLoop()
    {
        try
        {
            asio::io_context io_context(1);

            tcp::acceptor acceptor{io_context, {ip::make_address(address), port}};
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
                
                try
                {
                    ptrRouter->GetEndPoint(httpRequestContext).ProcessRequest(httpRequestContext);
                }
                catch(const std::exception exception)
                {
                    std::cerr << "request process error: " << exception.what() << std::endl;
                }


                beast::ostream(response.body()) << httpRequestContext.responseStream.str();

                http::write(
                    socket,
                    response);

                socket.shutdown(tcp::socket::shutdown_send);
            }
            catch (const std::exception exception)
            {
                std::cerr << "request process error: " << exception.what() << std::endl;
            }
        }
    }
public:
    template <class ... Args>
    WebServer(const Args& ... args) :
    address    (Address(args ..., Address("0.0.0.0")).value),
    port       (Port   (args ..., Port          (80)).value),
    threads    (Threads(args ..., Threads        (1)).value),
    ptrRouter  (std::move(webserverlibhelpers::Selector<Router, Args ..., Router>::Result(args ..., Router(new routing::RouterNode {{"", new routing::StaticDocumentEndPoint("index.html")}}))))
    {
        ptrRouter = ptrRouter ? ptrRouter : ;
        std::cout << "address : " << address << std::endl;
        std::cout << "port    : " << port    << std::endl;
        std::cout << "threads : " << threads << std::endl;
        
        std::thread(&WebServer::ListenLoop, this).detach();
        for (unsigned int i = 0; i < threads; i++)
            std::thread(&WebServer::RequestProcessorLoop, this).detach();
    }

    virtual ~WebServer()
    { delete ptrRouter; }
};