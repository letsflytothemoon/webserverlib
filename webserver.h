#pragma once
#include <iostream>
#include <fstream>
#include <thread>
#include <mutex>
#include <queue>
#include <map>
#include <functional>
#include <utility>
#include <memory>
#include <regex>
#include <boost/beast.hpp>
#include <boost/algorithm/string.hpp>

namespace webserverlib
{
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace asio = boost::asio;
    namespace ip = asio::ip;
    using tcp = ip::tcp;

    namespace webserverlibhelpers
    {
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
        Request&                           request;
        std::queue<std::string>            route;
        std::stringstream                  responseStream;
        std::map<http::field, std::string> headers;
        std::string                        remote_address;
        HttpRequestContext(Request& request, std::string remote_address) :
        request(request),
        remote_address(std::move(remote_address))
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
        
        class RouterNode : public Router
        {
            std::map<std::string, std::shared_ptr<Router>> routes;
        public:
            RouterNode(std::initializer_list<std::pair<const std::string, std::shared_ptr<Router>>> init) :
            routes(init)
            { }

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
                std::string fullPath = pathStringStream.str();

                std::ifstream fstream(fullPath);
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
            method(method)
            { }

            void ProcessRequest(HttpRequestContext& context) override
            { std::invoke(method, context); }
        };

        template <class Controller>
        class ApiControllerEndPoint : public RouterEndPoint
        {
            std::unique_ptr<Controller> ptrController;
            typedef void (Controller::*PtrMethod)(HttpRequestContext&);
            PtrMethod ptrMethod;
        public:
            ApiControllerEndPoint(PtrMethod ptrMethod) :
            ptrMethod(ptrMethod),
            ptrController(std::make_unique<Controller>())
            {  }

            void ProcessRequest(HttpRequestContext& context) override
            { (ptrController.get()->*ptrMethod)(context); }
        };
    }

    struct Router
    {
        std::initializer_list<std::pair<const std::string, std::shared_ptr<routing::Router>>> init;
        Router(std::initializer_list<std::pair<const std::string, std::shared_ptr<routing::Router>>> init) : init(init)
        { }

        operator std::shared_ptr<routing::RouterNode>() const
        { return std::make_shared<routing::RouterNode>(init); }
    };

    std::shared_ptr<routing::StaticDocumentEndPoint> StaticDocumentEndPoint(std::string fileName)
    { return std::make_shared<routing::StaticDocumentEndPoint>(std::move(fileName)); }

    std::shared_ptr<routing::StaticDirectoryEndPoint> StaticDirectoryEndPoint(std::string dirName)
    { return std::make_shared<routing::StaticDirectoryEndPoint>(std::move(dirName)); }

    template <class Method>
    std::shared_ptr<routing::ApiEndPoint<Method>> ApiEndPoint(Method method)
    { return std::make_shared<routing::ApiEndPoint<Method>>(method); }

    template <class Controller>
    std::shared_ptr<routing::ApiControllerEndPoint<Controller>> ApiControllerEndPoint(void(Controller::*method)(HttpRequestContext&))
    { return std::make_shared<routing::ApiControllerEndPoint<Controller>>(method); }

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
            mutable std::shared_ptr<routing::Router> ptrRouter;
        public:
            SetRouter(routing::Router* ptrRouter) : ptrRouter(ptrRouter) { }

            SetRouter(std::shared_ptr<routing::RouterNode> router)
            {
                ptrRouter = router;
            }

            operator std::shared_ptr<routing::Router>() const
            {
                std::shared_ptr<routing::Router> result = ptrRouter;
                ptrRouter = nullptr;
                return result;
            }
            ~SetRouter() { }
        };

    protected:
        typename SetAddress::ValueType   address;
        typename SetPort   ::ValueType   port;
        typename SetThreads::ValueType   threads;
        std::shared_ptr<routing::Router> ptrRouter;

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
                    
                    HttpRequestContext httpRequestContext(request, std::move(socket.remote_endpoint().address().to_string()));
                    
                    try
                    { ptrRouter->GetEndPoint(httpRequestContext).ProcessRequest(httpRequestContext); }
                    catch(const std::exception& exception)
                    { std::cerr << "request process error: " << exception.what() << std::endl; }
                    
                    for(auto i = httpRequestContext.headers.begin(); i != httpRequestContext.headers.end(); i++)
                        response.set(i->first, i->second);

                    beast::ostream(response.body()) << httpRequestContext.responseStream.str();
                    
                    http::write(
                        socket,
                        response);

                    socket.shutdown(tcp::socket::shutdown_send);
                }
                catch (const std::exception& exception)
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
            std::thread(&WebServer::ListenLoop, this).detach();
            for (unsigned int i = 0; i < threads; i++)
                std::thread(&WebServer::RequestProcessorLoop, this).detach();
        }

        virtual ~WebServer() { }
    };
}