
//
//  utils.cpp
//  Proxy server
//
//  Created by Kurkin Dmitry on 23.11.15.
//  Copyright © 2015 Kurkin Dmitry. All rights reserved.
//

#include "utils.hpp"


client_t::client_t(server_t* server) {
    
    struct sockaddr addr;
    socklen_t socklen = sizeof(addr);
    socket = accept(server->get_socket(), &addr, &socklen);
    if (socket == -1) {
        perror("can't get accept client");
        throw "Error in eccept client";
    }
    
    const int set = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));  // NOSIGPIPE FOR SEND
}

client_t::client_t(in_addr address) {
    
    socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == -1) {
        perror("cant open sock");
        throw "can make new sock for server";
    }
    
    const int set = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));  // NOSIGPIPE FOR SEND
    
    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr(inet_ntoa(address));
    server.sin_port = htons(80);
    if (connect(socket,  (sockaddr *)&server, sizeof(server)) < 0) {
        perror("can't connect");
    }
    
    std::cout << "connected " << socket << " \n";
}

client_t::~client_t() {
    close(socket);
}

int client_t::get_socket() {
    return socket;
}

server_t::server_t(int port): port(port) {
    socket = ::socket(AF_INET, SOCK_STREAM, 0);
}

void server_t::bind_and_listen() {
    
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    server.sin_addr.s_addr = INADDR_ANY;

    int bnd = bind(socket, (sockaddr *)&server, sizeof(server));
    if (bnd == -1) {
        perror("Cant bind port");
        close(socket);
        throw std::exception();
    }
    
    if (listen(socket, SOMAXCONN) != 0) {
        close(socket);
        perror("Cant listen");
        throw std::exception();
    }
}

int server_t::get_socket() {
    return socket;
}