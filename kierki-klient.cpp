#include <iostream>
#include <cstring>
#include <vector>
#include <string>
#include <regex>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>

#include "common.h"
#include "err.h"

#define BUFFER_SIZE 1000

using namespace std;


struct ServerInfo {
    string ip;
    uint16_t port;
    int socket_fd;
};

struct ClientInfo {
    string ip;
    uint16_t port;
};

class Client {
public:
    Client(ServerInfo server_info, ClientInfo client_info, bool auto_place, char place);
    bool run();

private:
    ServerInfo server_info;
    ClientInfo client_info;
    pollfd pollfds[2];
    string write_buffer[2];
    string read_buffer[2];

    bool auto_place;
    bool put_card;
    bool sent_iam;
    char place;
    char starting_player;
    int return_code = 1;
    int trick_number;
    int type;

    vector<Card> cards;
    vector<Card> trick_cards;
    Card card_to_put;
    string log;

    void handle_server_messages();
    void handle_client_messages();
    void send_messages_to_server();
    void remove_card(Card card);
    void choose_card();
    bool check_card(Card card);
    bool is_card(string message);
    bool correct_message(string message);
    pair<int, vector<Card>> parse_trick_or_taken(string message);
};


Client::Client(ServerInfo server_info, ClientInfo client_info, bool auto_place, char place) {
    this->server_info = server_info;
    this->client_info = client_info;
    this->auto_place = auto_place;
    this->place = place;
    this->sent_iam = false;
    this->put_card = false;
    this->pollfds[0] = {server_info.socket_fd, POLLOUT, 0}; // Server socket for writing
    if (this->auto_place) this->pollfds[1] = {-1, 0, 0};
    else this->pollfds[1] = {STDIN_FILENO, POLLIN, 0}; // STDIN for input if not auto_place
}

bool Client::run() {
    char buffer[BUFFER_SIZE];
    while (true) {
        for (int i = 0; i < 2; i++) this->pollfds[i].revents = 0;
        int poll_status = poll(this->pollfds, 2, -1);
        if (poll_status < 0) syserr("poll");

        for (int i = 0; i < 2; i++) {
            if (this->pollfds[i].revents & POLLIN) {
                ssize_t message_length = read(this->pollfds[i].fd, buffer, BUFFER_SIZE);
                if (message_length < 0) continue;
                if (message_length == 0) {
                    close(this->pollfds[i].fd);
                    if (this->pollfds[1].revents & POLLOUT && this->write_buffer[1].size() > 0) {
                        ssize_t message_length = write(this->pollfds[1].fd, this->write_buffer[1].c_str(), this->write_buffer[1].size());
                        if (message_length < 0) continue;
                    }
                    return this->return_code;
                }
                this->read_buffer[i] += string(buffer, message_length);
            }

            if (this->pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(this->pollfds[i].fd);
                return this->return_code;
            }
        }

        this->handle_server_messages();
        if (!this->auto_place) this->handle_client_messages();
        this->send_messages_to_server();

        // Write to server socket
        for (int i = 0; i < 2; i++) {
            if (this->pollfds[i].revents & POLLOUT && this->write_buffer[i].size() > 0) {
                ssize_t message_length = write(this->pollfds[i].fd, this->write_buffer[i].c_str(), this->write_buffer[i].size());
                if (message_length < 0) continue;
                this->write_buffer[i] = this->write_buffer[i].substr(message_length);
            }
        }

        // Update pollfds
        for (int i = 0; i < 2; i++) {
            this->pollfds[i].events = POLLIN;
            if (this->write_buffer[i].size() > 0) {
                this->pollfds[i].events |= POLLOUT;
            }
        }
    }
}

void Client::handle_server_messages() {
    string message = extract_message(this->read_buffer[0]);
    while (message != "") {
        if (this->auto_place) cout << "[" + server_info.ip + ":" + to_string(server_info.port) + "," + client_info.ip + ":" + to_string(client_info.port) + ',' + get_timestamp() + "] " + message + "\r\n";
        if (!this->correct_message(message)) message = extract_message(this->read_buffer[0]);
        if (message.starts_with("BUSY")) {
            if (!this->auto_place) {
                string response = "Place busy, list of busy places received: ";
                for (int i = 4; i < (int)message.size(); i++) {
                    response += message[i] + ", ";
                }
                if (message.size() > 4) {
                    response.pop_back();
                    response.pop_back();
                }
                response += ".\n";
                this->write_buffer[1] += response;
            }
        }
        else if (message.starts_with("DEAL")) {
            this->type = message[4] - '0';
            this->starting_player = message[5];
            this->cards = string_to_card_vector(message.substr(6));
            this->log = "";
            if (!this->auto_place) {
                string response = "New deal " + to_string(this->type) + ": starting place " + this->starting_player + ", your cards: ";
                for (int i = 0; i < (int)this->cards.size(); i++) {
                    response += card_to_string(this->cards[i]) + ", ";
                }
                response.pop_back();
                response.pop_back();
                response += ".\n";
                this->write_buffer[1] += response;
            }
        } else if (message.starts_with("TAKEN")) {
            char winner = message.back();
            message.pop_back();
            pair<int, vector<Card>> taken = parse_trick_or_taken(message);
            int our_card = place_number(this->place) - place_number(this->starting_player);
            if (our_card < 0) our_card += 4;
            this->remove_card(taken.second[our_card]);
            this->starting_player = winner;
            if (!this->auto_place) {
                string response = "A trick " + to_string(taken.first) + " is taken by " + winner + ", cards ";
                for (int i = 0; i < (int)taken.second.size(); i++) {
                    response += card_to_string(taken.second[i]) + ", ";
                    this->log += card_to_string(taken.second[i]) + ", ";
                }
                response.pop_back();
                response.pop_back();
                response += ".\n";
                this->log.pop_back();
                this->log.pop_back();
                this->log += "\n";
                this->write_buffer[1] += response;
            }
        } else if (message.starts_with("TRICK")) {
            pair<int, vector<Card>> trick = parse_trick_or_taken(message);
            this->trick_number = trick.first;
            this->trick_cards = trick.second;
            this->put_card = true;
            this->card_to_put.value = 0;
            if (!this->auto_place) {
                string response = "Trick: (" + to_string(this->trick_number) + ") ";
                for (int i = 0; i < (int)this->trick_cards.size(); i++) {
                    response += card_to_string(this->trick_cards[i]) + ", ";
                }
                if (this->trick_cards.size() > 0) {
                    response.pop_back();
                    response.pop_back();
                }
                response += "\n";
                response += "Available: ";
                for (int i = 0; i < (int)this->cards.size(); i++) {
                    response += card_to_string(this->cards[i]) + ", ";
                }
                response.pop_back();
                response.pop_back();
                response += "\n";
                this->write_buffer[1] += response;
            }
        } else if (message.starts_with("SCORE")) {
            if (!this->auto_place) {
                string response = "The scores are:\n";
                char place = message[5];
                int p = 6;
                int score = 0;
                while (p < (int)message.size()) {
                    score += message[p] - '0';
                    if (p + 1 == (int)message.size() || message[p + 1] > '9') {
                        response += place;
                        response += " | " + to_string(score) + "\n";
                        score = 0;
                        if (p + 1 != (int)message.size()) place = message[p + 1];
                        p++;
                    }
                    else score *= 10;
                    p++;
                } 
                this->write_buffer[1] += response;
            }
        } else if (message.starts_with("TOTAL")) {
            if (!this->auto_place) {
                string response = "The total scores are:\n";
                int p = 6;
                int score = 0;
                while (p < (int)message.size()) {
                    score += message[p] - '0';
                    if (p + 1 == (int)message.size() || message[p + 1] > '9') {
                        response += place;
                        response += " | " + to_string(score) + "\n";
                        score = 0;
                        if (p + 1 != (int)message.size()) place = message[p + 1];
                        p++;
                    }
                    else score *= 10;
                    p++;
                } 
                this->write_buffer[1] += response;
            }
            this->cards.clear();
        } else if (message.starts_with("WRONG")) {
            int number = message[5] - '0';
            if (message.size() == 7) number = 10 + message[6] - '0';
            if (!this->auto_place) {
                string response = "Wrong message received in trick " + to_string(number) + ".\n";
                this->write_buffer[1] += response;
            }
        } else continue;
        message = extract_message(this->read_buffer[0]);
    }
}

void Client::handle_client_messages() {
    string message = extract_stdin_message(this->read_buffer[1]);
    while (message != "") {
        if (message == "cards") {
            string response = "Your cards: ";
            for (int i = 0; i < (int)this->cards.size(); i++) {
                response += card_to_string(this->cards[i]) + ", ";
            }
            response.pop_back();
            response.pop_back();
            response += ".\n";
            this->write_buffer[1] += response;
        } else if (message == "tricks") {
            this->write_buffer[1] += this->log;
        } else if (this->is_card(message.substr(1)) && this->put_card) {
            this->card_to_put = string_to_card(message.substr(1));
            if (!this->check_card(this->card_to_put)) {
                this->card_to_put.value = 0;
                this->write_buffer[1] += "Wrong card.\n";
            }
        } else {
            string response = "Unknown command.\n";
            this->write_buffer[1] += response;
        }
        message = extract_stdin_message(this->read_buffer[1]);
    }
}

void Client::send_messages_to_server() {
    if (!this->sent_iam) {
        string response = "IAM" + string(1, this->place) + "\r\n";
        this->write_buffer[0] += response;
        this->sent_iam = true;
        if (this->auto_place) cout << "[" + server_info.ip + ":" + to_string(server_info.port) + "," + client_info.ip + ":" + to_string(client_info.port) + ',' + get_timestamp() + "] " + response;
    }
    if (this->put_card) {
        if (this->auto_place) this->choose_card();
        if (this->card_to_put.value == 0) return;
        string response = "TRICK" + to_string(this->trick_number) + card_to_string(this->card_to_put) + "\r\n";
        this->write_buffer[0] += response;
        this->put_card = false;
        if (this->auto_place) cout << "[" + server_info.ip + ":" + to_string(server_info.port) + "," + client_info.ip + ":" + to_string(client_info.port) + ',' + get_timestamp() + "] " + response;
    }
}

void Client::remove_card(Card card) {
    for (int i = 0; i < (int)this->cards.size(); i++) {
        if (this->cards[i].color == card.color && this->cards[i].value == card.value) {
            this->cards.erase(this->cards.begin() + i);
            break;
        }
    }
}

void Client::choose_card() {
    if (this->trick_cards.size() == 0) {
        this->card_to_put = this->cards[0];
        return;
    }
    char color = this->trick_cards[0].color;
    for (int i = 0; i < (int)this->cards.size(); i++) {
        if (this->cards[i].color == color) {
            this->card_to_put = this->cards[i];
            return;
        }
    }
    this->card_to_put = this->cards[0];
}

bool Client::check_card(Card card) {
    bool has_card = false;
    for (int i = 0; i < (int)this->cards.size(); i++) {
        if (card.value == this->cards[i].value && card.color == this->cards[i].color) has_card = true;
    }
    if ((int)this->trick_cards.size() == 0) return has_card;
    if (this->trick_cards[0].color == card.color) return true;
    for (int i = 0; i < (int)this->cards.size(); i++) {
        if (this->trick_cards[0].color == this->cards[i].color) return false;
    }
    return true;
}

bool Client::is_card(string message) {
    regex pattern(R"((10|[2-9]|J|Q|K|A)(C|D|H|S))");
    return regex_match(message, pattern);
}

bool Client::correct_message(string message) {
    this->return_code = 1;
    regex pattern("^BUSY[NEWS]{1,4}$");
    if (regex_match(message, pattern)) return true;
    pattern = regex("^DEAL[1-7][NSWE]((10|[2-9]|[JQKA])[CDSH]){13}$");
    if (regex_match(message, pattern)) return true;
    pattern = regex("^TAKEN(1[0-3]|[1-9])((10|[2-9]|[JQKA])[CDSH]){4}[NSWE]$");
    if (regex_match(message, pattern)) return true;
    pattern = regex("^TRICK(1[0-3]|[1-9])((10|[2-9]|[JQKA])[CDSH]){0,3}$");
    if (regex_match(message, pattern)) return true;
    pattern = regex("^WRONG(1[0-3]|[1-9])$");
    if (regex_match(message, pattern)) return true;
    pattern = regex("^SCORE[NESW][0-9]+[NESW][0-9]+[NESW][0-9]+[NESW][0-9]+$");
    if (regex_match(message, pattern)) return true;
    pattern = regex("^TOTAL[NESW][0-9]+[NESW][0-9]+[NESW][0-9]+[NESW][0-9]+$");
    if (regex_match(message, pattern)) {
        this->return_code = 0;
        return true;
    }
    return false;
}

pair<int, vector<Card>> Client::parse_trick_or_taken(string message) {
    pair<int, vector<Card>> result;
    if (message.size() == 6) result.first = message[5] - '0';
    else if (message.size() == 7) result.first = 10 + message[6] - '0';
    else {
        if (is_color(message[7])) {
            result.first = message[5] - '0';
            result.second = string_to_card_vector(message.substr(6)); 
        }
        else if (is_color(message[8])) {
            if (message[7] == '0') {
                result.first = message[5] - '0';
                result.second = string_to_card_vector(message.substr(6)); 
            }
            else {
                result.first = 10 + message[6] - '0';
                result.second = string_to_card_vector(message.substr(7));
            }
        }
        else {
            result.first = 10 + message[6] - '0';
            result.second = string_to_card_vector(message.substr(7));
        }
    }
    return result;
}

ServerInfo get_server_address(const char *host, uint16_t port, bool ipv4, bool ipv6) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    if (ipv4) hints.ai_family = AF_INET;
    else if (ipv6) hints.ai_family = AF_INET6;
    else hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *address_result;
    int errcode = getaddrinfo(host, to_string(port).c_str(), &hints, &address_result);
    if (errcode != 0) {
        fatal("getaddrinfo: %s", gai_strerror(errcode));
    }

    ServerInfo server_info;
    server_info.socket_fd = socket(address_result->ai_family, address_result->ai_socktype, address_result->ai_protocol);
    if (server_info.socket_fd == -1) {
        syserr("socket");
    }
    if (connect(server_info.socket_fd, address_result->ai_addr, address_result->ai_addrlen) == -1) {
        syserr("connect");
    }
    fcntl(server_info.socket_fd, F_SETFL, O_NONBLOCK);

    char server_ip[INET6_ADDRSTRLEN];
    if (address_result->ai_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)address_result->ai_addr;
        inet_ntop(AF_INET, &(addr_in->sin_addr), server_ip, INET_ADDRSTRLEN);
        server_info.ip = string(server_ip);
        server_info.port = ntohs(addr_in->sin_port);
    } else if (address_result->ai_family == AF_INET6) {
        struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)address_result->ai_addr;
        inet_ntop(AF_INET6, &(addr_in6->sin6_addr), server_ip, INET6_ADDRSTRLEN);
        server_info.ip = string(server_ip);
        server_info.port = ntohs(addr_in6->sin6_port);
    }

    freeaddrinfo(address_result);
    return server_info;
}

ClientInfo get_client_info(int socket_fd) {
    struct sockaddr_storage client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    if (getsockname(socket_fd, (struct sockaddr *)&client_addr, &client_addr_len) == -1) {
        syserr("getsockname");
    }

    ClientInfo client_info;
    char client_ip[INET6_ADDRSTRLEN];
    if (client_addr.ss_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)&client_addr;
        inet_ntop(AF_INET, &(addr_in->sin_addr), client_ip, INET_ADDRSTRLEN);
        client_info.ip = string(client_ip);
        client_info.port = ntohs(addr_in->sin_port);
    } else if (client_addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)&client_addr;
        inet_ntop(AF_INET6, &(addr_in6->sin6_addr), client_ip, INET6_ADDRSTRLEN);
        client_info.ip = string(client_ip);
        client_info.port = ntohs(addr_in6->sin6_port);
    }

    return client_info;
}

int main(int argc, char *argv[]) {
    string host = "";
    uint16_t port = 0;
    bool ipv4 = false;
    bool ipv6 = false;
    char place = '0';
    bool auto_place = false;

    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-h") {
            if (i + 1 >= argc) fatal("Missing argument for -h");
            else host = argv[++i];
        } else if (arg == "-p") {
            if (i + 1 >= argc) fatal("Missing argument for -p");
            else port = read_port(argv[++i]);
        } else if (arg == "-4") {
            ipv4 = true;
            ipv6 = false;
        } else if (arg == "-6") {
            ipv4 = false;
            ipv6 = true;
        } else if (arg == "-N") place = 'N';
        else if (arg == "-E") place = 'E';
        else if (arg == "-S") place = 'S';
        else if (arg == "-W") place = 'W';
        else if (arg == "-a") auto_place = true;
        else fatal("Incorrect arguments");
    }

    if (host == "") fatal("Missing host name");
    if (port == 0) fatal("Missing port number");
    if (place == '0') fatal("Missing place");

    ServerInfo server_info = get_server_address(host.c_str(), port, ipv4, ipv6);
    ClientInfo client_info = get_client_info(server_info.socket_fd);

    Client client(server_info, client_info, auto_place, place);
    return client.run();
}
