//
//  proxy.cpp
//  Proxy server
//
//  Created by Kurkin Dmitry on 21.11.15.
//  Copyright © 2015 Kurkin Dmitry. All rights reserved.
//

#include <assert.h>
#include <sys/errno.h>

#include "proxy.hpp"
#include "throw_error.h"
#include "new_http_handler.hpp"

namespace
{
    void write_some(tcp_client& dest, io_queue& queue, int ident)
    {
        if (dest.msg_queue.empty()) {
            queue.delete_event_handler(ident, EVFILT_WRITE);
            return;
        }
        write_part part = dest.msg_queue.front();
        dest.msg_queue.pop_front();
        size_t writted = ::write(ident, part.get_part_text(), part.get_part_size());
        if (writted == -1) {
            if (errno != EPIPE) {
                throw_error(errno, "write()");
            } else {
                if (part.get_part_size() != 0) {
                    dest.msg_queue.push_front(part); // no strong exception guarantee
                }
            }
        } else {
            part.writted += writted;
            if (part.get_part_size() != 0) {
                dest.msg_queue.push_front(part);
            }
        }
    }

    constexpr const timer::clock_t::duration timeout = std::chrono::seconds(120);
}

void proxy_server::resolve(std::unique_ptr<parse_state> state)
{
    std::lock_guard<std::mutex> lk(queue_mutex);
    host_names.push_back(std::move(state));
    queue_cond.notify_one();
}

proxy_server::proxy_server(io_queue& queue, int port, size_t resolvers_num): server(server_socket(port)), queue(queue), cache(10000), addr_cache(10000)
{
    server.bind_and_listen();

    for (size_t i = 0; i < resolvers_num; i++) {
        resolvers.push_back(std::thread(resolver));
    }
    
    queue.add_event_handler(server.getfd(), EVFILT_READ, connect_client);
    queue.add_event_handler(USER_EVENT_IDENT, EVFILT_USER, EV_CLEAR, host_resolfed);
}

proxy_server::~proxy_server()
{
    queue.delete_event_handler(server.getfd(), EVFILT_READ);
    queue.delete_event_handler(USER_EVENT_IDENT, EVFILT_USER);
}

proxy_server::proxy_tcp_connection::proxy_tcp_connection(proxy_server& proxy, io_queue& queue, tcp_client&& client)
    : tcp_connection(queue, std::move(client))
    , timer(this->queue.get_timer(), timeout, [this, &proxy]() {
        std::cout << "timeout for " << get_client_socket() << "\n";
        proxy.connections.erase(this);
    })
    , proxy(proxy)
{}

proxy_server::proxy_tcp_connection::~proxy_tcp_connection()
{
    if (state)
        state->canceled = true;
}

std::string proxy_server::proxy_tcp_connection::get_host() const noexcept
{
    return request->get_host();
}

void proxy_server::proxy_tcp_connection::set_client_addr(const sockaddr& addr)
{
    client_addr = addr;
}

void proxy_server::proxy_tcp_connection::set_client_addr(const sockaddr&& addr)
{
    client_addr = std::move(addr);
}

void proxy_server::proxy_tcp_connection::connect_to_server()
{
    if (get_server_socket() != -1) {
        if (request->get_host() == host)
        {
            std::cout << "keep-alive is working!\n";
            try_to_cache();
            response.release();
            response = nullptr;
            URI = request->get_URI();
            return;
        } else {
            deregistrate(server);
        }
    }
    
    server = tcp_client(client_socket(client_addr));
    host = request->get_host();
    URI = request->get_URI();
 
    set_server_on_read_write(
        [this](struct kevent event)
        { server_on_read(event); },
        [this](struct kevent event)
        { server_on_write(event); });
}

void proxy_server::proxy_tcp_connection::client_on_read(struct kevent event)
{
    if (event.flags & EV_EOF)
    {
        std::cout << "EV_EOF from " << event.ident << " client\n";
        proxy.connections.erase(this);
    } else
    {
        timer.restart(queue.get_timer(), timeout);
        char buff[event.data];
        
        std::cout << "read request of " << event.ident << "\n";
        
        size_t size = read(get_client_socket() , buff, event.data);
        if (size == static_cast<size_t>(-1)) {
            throw_error(errno, "read()");
        }
        
        std::cout << "readed " << std::string{buff, size} << "\n";
        
        if (request) {
            request->add_part({buff, size});
        } else {
            request.reset(new struct request({buff, size}));
        }
        
        if (request->get_state() == BAD)
        {
            send(get_client_socket(), "HTTP/1.1 400 Bad Request\r\n\r\n", strlen("HTTP/1.1 400 Bad Request\r\n\r\n"), 0);
            proxy.connections.erase(this);
            return;
        }
        
        if (request->get_state() == FULL_BODY)
        {
            auto host = get_host();
            std::string port = "80";
            if (host.find(":") != static_cast<size_t>(-1)) {
                size_t port_str = host.find(":");
                port = host.substr(port_str + 1);
                host = host.erase(port_str);
            }
            std::unique_lock<std::mutex> lk1(proxy.cache_mutex);
            if (proxy.addr_cache.contain(host + port) && request->get_method() != "CONNECT") {
                std::cout << "dns cache is working!\n";
                auto addr = proxy.addr_cache.get(host + port);
                lk1.unlock();
                set_client_addr(std::move(addr));
                connect_to_server();
                make_request();
            } else {
                lk1.unlock();
                std::cout << "push to resolve " << get_host() << request->get_URI() << "\n";
                auto temp = std::unique_ptr<parse_state>(new parse_state(this));
                state = temp.get();
                proxy.resolve(std::move(temp));
            }
        }
    }
}

void proxy_server::proxy_tcp_connection::client_on_write(struct kevent event)
{
    timer.restart(queue.get_timer(), timeout);
    write_some(client, queue, event.ident);
}

void proxy_server::proxy_tcp_connection::server_on_write(struct kevent event)
{
    timer.restart(queue.get_timer(), timeout);
    write_some(server, queue, event.ident);
}

void proxy_server::proxy_tcp_connection::server_on_read(struct kevent event)
{
    if (event.flags & EV_EOF && event.data == 0) {
        std::cout << "EV_EOF from " << event.ident << " server\n";
        try_to_cache();
        deregistrate(server);
        server = client_socket();
    } else {
        timer.restart(queue.get_timer(), timeout);
        char buff[event.data];
        size_t size = recv(get_server_socket(), buff, event.data, 0);
        if (size == -1) {
            if (errno == EAGAIN) {
                return;
            } else {
                throw_error(errno, "recv()");
            }
        }
        if (response == nullptr) {
            response.reset(new struct response({buff,size}));
        } else {
            response->add_part({buff, size});
        }
        write_to_client({buff, size});
    }
}

void proxy_server::proxy_tcp_connection::CONNECT_on_read(struct kevent event)
{
    if (event.flags & EV_EOF && event.data == 0) {
        proxy.connections.erase(this);
    } else {
        timer.restart(queue.get_timer(), timeout);
        char buff[event.data];
        size_t size = recv(event.ident, buff, event.data, 0);
        if (size == -1) {
            if (errno == EAGAIN) {
                return;
            } else {
                throw_error(errno, "recv()");
            }
        }
        if (get_client_socket() == event.ident) {
            write_to_server({buff, size});
        } else {
            write_to_client({buff, size});
        }
    }
}

void proxy_server::proxy_tcp_connection::make_request()
{
    if (!request->is_validating() && proxy.cache.contain(request->get_host() + request->get_URI())) {
        std::cout << "cache is working! for " << get_client_socket() << "\n"; // TODO: validate cache use if_match
        const struct response& cache_response =  proxy.cache.get(request->get_host() + request->get_URI());
        request.reset(cache_response.get_validating_request(request->get_URI(), request->get_host()));
        set_server_on_read_write([this, cache_response](struct kevent event){
            if (event.flags & EV_EOF && event.data == 0) {
                std::cout << "EV_EOF from " << event.ident << " server\n";
                try_to_cache();
                deregistrate(server);
                server = client_socket();
            } else {
                timer.restart(queue.get_timer(), timeout);
                char buff[event.data];
                size_t size = recv(get_server_socket(), buff, event.data, 0);
                if (size == -1)
                    throw_error(errno, "recv()");
                if (response == nullptr) {
                    response.reset(new struct response({buff,size}));
                } else {
                    response->add_part({buff, size});
                }
                if (response->get_state() >= FIRST_LINE) {
                    if (response->get_code() != "200") {
                        std::cout << "Not modified " << response->get_code() << "\n";
                        write_to_client(std::move(cache_response.get_text()));
                        set_server_on_read_write(
                                                 [this](struct kevent event)
                                                 {  if (event.flags & EV_EOF && event.data == 0) {
                                                        deregistrate(server);
                                                        server = client_socket();
                                                    } else {
                                                        timer.restart(queue.get_timer(), timeout);
                                                        char buff[event.data];
                                                        recv(get_server_socket(), buff, event.data, 0);}
                                                 },
                                                 [this](struct kevent event)
                                                 { server_on_write(event); });
                    } else {
                        std::cout << "Modified " << response->get_code() << "\n";
                        write_to_client({buff, size});
                        set_server_on_read_write(
                                                 [this](struct kevent event)
                                                 { server_on_read(event); },
                                                 [this](struct kevent event)
                                                 { server_on_write(event); });
                    }
                }
            }
        },
                                [this](struct kevent event) {
                                     server_on_write(event);
                                 });
    }
    
    std::cout << "tcp_pair: client: " << get_client_socket() << " server: " << get_server_socket() << "\n";
    
    std::cout << request->get_request_text() << "\n";
    write_to_server(std::move(request->get_request_text()));
    request.release();
}

void proxy_server::proxy_tcp_connection::try_to_cache()
{
    if (response != nullptr && response->is_cacheable()) {
        std::cout << "add to cache: " << host + URI  <<  " " << response->get_header("ETag") << "\n";
        proxy.cache.put(host + URI, *response);
    }
}
