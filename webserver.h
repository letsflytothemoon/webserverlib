#pragma once
#include <iostream>
#include <fstream>
#include <thread>
#include <mutex>
#include <queue>
#include <map>
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

namespace webserverexceptions
{
    class WebServerException : public std::exception
    { };

    class NotFoundException : public WebServerException
    {
    public:
        const char* what() const noexcept override
        { return "404 - not found"; }
    };

    class BadRequestException : public WebServerException
    {
    public:
        const char* what() const noexcept override
        { return "bad request"; }
    };
}

using Body     = http::dynamic_body;
using Request  = http::request<Body>;
using Response = http::response<Body>;

class HttpRequestContext
{
public:
    Request&                request;
    std::queue<std::string> route;
    std::stringstream       responseStream;

    HttpRequestContext(Request& request) :
    request(request)
    {
        std::vector<std::string> path;
        boost::split(path, request.target(), boost::is_any_of("/"));
        for(auto i = ++path.begin(); i != path.end(); i++)
            if(*i != "")
                route.push(*i);
    }

    std::string GetPathStep()
    {
        if(route.empty())
            return "";
        struct Poper
        {
            std::queue<std::string>& queue;
            Poper(std::queue<std::string>& queue) : queue(queue) { }
            ~Poper() { queue.pop(); }
        } poper(route);
        return std::move(route.front());
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

    template <class ... Routers>
    class RouterNode : public Router
    {
        std::map<std::string, routing::Router*> routes;
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
                throw webserverexceptions::NotFoundException();
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
                    throw webserverexceptions::BadRequestException();
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

    template <class Method>
    class ApiEndPoint : public RouterEndPoint
    {
        Method method;
    public:
        ApiEndPoint(Method method) :
        method(std::move(method))
        { }

        void ProcessRequest(HttpRequestContext& context) override
        { method(context); }
    };

    template <class Method>
    ApiEndPoint<Method>* CreateApiEndPoint(Method method)
    { return new ApiEndPoint<Method>(std::move(method)); }
    //---------------------------------------------------------------------------
    template <class Controller>
    class ApiControllerEndPoint : public RouterEndPoint
    {
        Controller* ptrController;
        typedef void (Controller::*PtrMethod)(HttpRequestContext&);
        PtrMethod ptrMethod;
    public:
        ApiControllerEndPoint(PtrMethod ptrMethod, Controller* ptrController = nullptr) :
        ptrController(ptrController),
        ptrMethod(ptrMethod)
        { }

        void ProcessRequest(HttpRequestContext& context) override
        {
            Controller* controller = nullptr;
            ((ptrController ? ptrController : controller = new Controller())->*ptrMethod)(context);
            delete controller;
        }
    };

    template <class Controller>
    ApiControllerEndPoint<Controller>* CreateApiControllerEndPoint(void(Controller::*method)(HttpRequestContext&))
    { return new ApiControllerEndPoint<Controller>(method); }
    //---------------------------------------------------------------------------
}


class WebServer
{
protected:
    template <class ValueT>
    struct ClassId
    { typedef ValueT ValueType; };

    struct AddressId : ClassId<std::string     > { };
    struct PortId    : ClassId<unsigned short  > { };
    struct ThreadsId : ClassId<unsigned int    > { };
public:
    typedef webserverlibhelpers::Prop<AddressId> SetAddress;
    typedef webserverlibhelpers::Prop<PortId   > SetPort;
    typedef webserverlibhelpers::Prop<ThreadsId> SetThreads;

    class SetRouter
    {
        mutable routing::Router* ptrRouter;
    public:
        SetRouter(SetRouter&& other)
        {
            ptrRouter = other.ptrRouter;
            other.ptrRouter = nullptr;
        }

        SetRouter(routing::Router* ptrRouter) : ptrRouter(ptrRouter) { }

        operator routing::Router*() const
        {
            routing::Router* result = ptrRouter;
            ptrRouter = nullptr;
            return result;
        }
        ~SetRouter() { delete ptrRouter; }
    };

protected:
    typename SetAddress    ::ValueType address;
    typename SetPort       ::ValueType port;
    typename SetThreads    ::ValueType threads;
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

                std::cout << "rquest processor: target: " << request.target() << std::endl;

                Response response;
                response.version(request.version());

                HttpRequestContext httpRequestContext(request);
                
                try
                {
                    ptrRouter->GetEndPoint(httpRequestContext).ProcessRequest(httpRequestContext);
                }
                catch(const std::exception& exception)
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
    address    (SetAddress(args ..., SetAddress("0.0.0.0")).value),
    port       (SetPort   (args ..., SetPort          (80)).value),
    threads    (SetThreads(args ..., SetThreads        (1)).value),
    ptrRouter  (webserverlibhelpers::Selector<SetRouter, Args ..., SetRouter>::Result(args ..., SetRouter(new routing::StaticDocumentEndPoint("index.html"))))
    {
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