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

    using Body     = http::dynamic_body;
    using Request  = http::request<Body>;
    using Response = http::response<Body>;

    class HttpRequestContext
    {
        std::queue<std::string>            route;
    public:
        Request&                           request;
        std::string                        remote_address;

        HttpRequestContext(Request& request, std::string remote_address) :
        request(request),
        remote_address(std::move(remote_address))
        {
            std::vector<std::string> path;
            boost::split(path, request.target(), boost::is_any_of("/"));
            for (auto i = ++path.begin(); i != path.end(); i++)
                if(*i != "")
                    route.push(*i);
        }

        std::string GetRouteStep()
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

        operator Request&() { return request; }
    };

    struct File
    { std::string name; };

    struct Directory
    { std::string name; };

    class Router
    {
         std::function<Response(HttpRequestContext&)> action;
    public:
        Response Act(HttpRequestContext& requestContext) const
        { return action(requestContext); }

        Router(std::initializer_list<std::pair<const std::string, Router>> init)
        {
            std::map<std::string, Router> initRoutes;
            for(auto i = init.begin(); i != init.end(); i++)
                initRoutes.emplace(std::make_pair(i->first, i->second));
            
            action = [routes { std::move(initRoutes) } ](HttpRequestContext& requestContext)
            {
                auto routeFindIterator = routes.find(requestContext.GetRouteStep());
                if(routeFindIterator == routes.end())
                    throw http::status::not_found;
                return routeFindIterator->second.Act(requestContext);
            };
        }

        template <class Action>
        Router(Action action, std::enable_if_t<std::is_invocable_v<Action, HttpRequestContext&>, int> = 0) :
        action(action)
        { }

        template <class Controller>
        Router(Response(Controller::*action)(HttpRequestContext&)) :
        action([action](HttpRequestContext& requestContext)
        {
            Controller controller;
            return (controller.*action)(requestContext);
        })
        { }

        Router(File file) :
        action([fileName{std::move(file.name)}](HttpRequestContext& context)
        {
            Response response;
            response.version(context.request.version());
            beast::ostream(response.body()) << std::ifstream(fileName).rdbuf();
            return response;
        })
        { }

        Router(Directory directory) :
            action([dirName{ std::move(directory.name) }](HttpRequestContext& context)
        {
            Response response;
            response.version(context.request.version());
            std::stringstream pathStringStream;
            pathStringStream << dirName;
            std::string routeStep;
            while((routeStep = std::move(context.GetRouteStep())) != "")
            {
                if(routeStep == "..")
                    throw http::status::bad_request;
                pathStringStream << "/" << routeStep;
            }
            beast::ostream(response.body()) << std::ifstream(pathStringStream.str()).rdbuf();
            return response;
        })
        { }
    };

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

    protected:
        typename SetAddress::ValueType   address;
        typename SetPort   ::ValueType   port;
        typename SetThreads::ValueType   threads;
        Router                           router;

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
                    
                    HttpRequestContext httpRequestContext(request, std::move(socket.remote_endpoint().address().to_string()));
                    
                    Response response;

                    try
                    {
                        response = std::move(router.Act(httpRequestContext));
                    }
                    catch (beast::http::status status)
                    {
                        response.result(status);
                        beast::ostream(response.body()) << beast::http::obsolete_reason(status);
                    }
                    catch (const std::exception&)
                    {
                        //write to log or something
                        response.result(http::status::internal_server_error);
                        beast::ostream(response.body()) << beast::http::obsolete_reason(http::status::internal_server_error);
                    }
                    
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
        address  (SetAddress(args ..., SetAddress("0.0.0.0")).value),
        port     (SetPort   (args ..., SetPort          (80)).value),
        threads  (SetThreads(args ..., SetThreads        (1)).value),
        router   (webserverlibhelpers::Selector<Router, Args ..., Router>::Result(args ..., [](HttpRequestContext& context)
        {
            Response response;
            response.version(context.request.version());
            beast::ostream(response.body()) << "<!doctype html><html><head><title>it works!</title><body><h1>It works!</h1></body></html>";
            return response;
        }))
        {
            std::thread(&WebServer::ListenLoop, this).detach();
            for (unsigned int i = 0; i < threads; i++)
                std::thread(&WebServer::RequestProcessorLoop, this).detach();
        }

        virtual ~WebServer() { }
    };
}