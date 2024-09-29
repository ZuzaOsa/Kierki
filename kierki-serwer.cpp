#include <iostream>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <regex>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "common.h"
#include "err.h"

#define BUFFER_SIZE      1000
#define QUEUE_LENGTH     5

using namespace std;

struct Round {
    int type;
    char starting_player;
    vector<Card> player_cards[4];
    string player_cards_string[4];
};

struct ClientInfo {
    string ip;
    uint16_t port;
    string write_buffer;
    string read_buffer;
    int place = -1;
    bool disconnect = false;
};

class Player {
public:
    int id = 0;
    int round_points = 0;
    int total_points = 0;
    void remove_card(Card card);
    void give_cards(vector<Card> cards);
    bool has_card(Card card);
    bool has_color(char color);
private:
    vector<Card> cards;
};

void Player::remove_card(Card card) {
    for (int i = 0; i < (int)this->cards.size(); i++) {
        if (this->cards[i].color == card.color && this->cards[i].value == card.value) {
            this->cards.erase(this->cards.begin() + i);
            break;
        }
    }
}

void Player::give_cards(vector<Card> cards) {
    this->cards = cards;
}

bool Player::has_card(Card card) {
    for (int i = 0; i < (int)this->cards.size(); i++) {
        if (this->cards[i].color == card.color && this->cards[i].value == card.value) return true;
    }
    return false;
}

bool Player::has_color(char color) {
    for (int i = 0; i < (int)this->cards.size(); i++) {
        if (this->cards[i].color == color) return true;
    }
    return false;
}

class Game {
public:
    Game(uint16_t port, string file, int timeout);
    void run();
private:
    bool game_over = false;
    bool timeout_passed = true;
    int connected_clients = 0;
    int current_player;
    int phase = 0;
    int round = 0;
    int timeout;
    int trick_number;

    Player players[4];
    vector<Card> trick_cards;
    vector<ClientInfo> clients;
    vector<Round> rounds;
    vector<pollfd> pollfds;
    vector<string> log;

    void create_server_socket(uint16_t port);
    void handle_messages();
    bool is_iam(string message);
    pair<int, string> is_trick(string message);
    void send_trick();
    void send_taken();
    void send_score_and_total();
    void reconnect_player(int place);
    bool check_trick(string message, int id);
    void count_points(char winner, int round_type);
    void send_message(int id, string message);
};

Game::Game(uint16_t port, string file, int timeout) {
    ifstream infile(file);
    if (!infile) {
        fatal("Cannot open file %s", file.c_str());
    }
    string line;
    while (getline(infile, line)) {
        Round r;
        r.type = line[0] - '0';
        r.starting_player = line[1];
        for (int i = 0; i < 4; ++i) {
            getline(infile, line);
            r.player_cards[i] = string_to_card_vector(line);
            r.player_cards_string[i] = line;
        }
        this->rounds.push_back(r);
    }
    infile.close();
    this->create_server_socket(port);
    this->timeout = timeout;
}

void Game::run() {
    char buffer[BUFFER_SIZE];

    while(true) {
        for (int i = 0; i < (int)this->pollfds.size(); i++) this->pollfds[i].revents = 0;
        this->timeout_passed = false;
        int poll_status = poll(this->pollfds.data(), this->pollfds.size(), this->timeout);
        if (poll_status < 0) syserr("poll");
        if (poll_status == 0) {
            for (int i = 1; i < (int)this->pollfds.size(); i++) {
                if (this->clients[i].place == -1) {
                    close(this->pollfds[i].fd);
                    this->pollfds[i].fd = -1;
                }
            }
            this->timeout_passed = true;
        }
        //Read from all clients
        for (int i = 1; i < (int)this->pollfds.size(); i++) {
            if (this->pollfds[i].fd == -1) continue;
            if (this->pollfds[i].revents & POLLIN) {
                ssize_t message_length = read(this->pollfds[i].fd, buffer, BUFFER_SIZE);
                if (message_length < 0) continue;
                if (message_length == 0) {
                    close(this->pollfds[i].fd);
                    this->pollfds[i].fd = -1;
                    continue;
                }
                this->clients[i].read_buffer += string(buffer, message_length);               
            }
            if (this->pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(this->pollfds[i].fd);
                this->pollfds[i].fd = -1;
            }
        }
        this->handle_messages();
        //Remove disconnected clients
        for (int i = 1; i < (int)this->pollfds.size(); i++) {
            if (this->clients[i].disconnect && this->clients[i].write_buffer.size() == 0) {
                close(this->pollfds[i].fd);
                this->pollfds[i].fd = -1;
            }
                
            if (this->pollfds[i].fd == -1) {
                if (this->clients[i].place != -1) {
                    this->players[this->clients[i].place].id = 0;
                    this->connected_clients--;
                }
                this->pollfds.erase(this->pollfds.begin() + i);
                this->clients.erase(this->clients.begin() + i);
                i--;
            }
        }
        //Write to all clients
        for (int i = 1; i < (int)this->pollfds.size(); i++) {
            if (this->pollfds[i].revents & POLLOUT) {
                ssize_t message_length = write(this->pollfds[i].fd, this->clients[i].write_buffer.c_str(), this->clients[i].write_buffer.size());
                if (message_length < 0) continue;
                this->clients[i].write_buffer = this->clients[i].write_buffer.substr(message_length);
            }
        }
        //Update pollfds
        for (int i = 1; i < (int)this->pollfds.size(); i++) {
            if (this->clients[i].disconnect) {
                this->pollfds[i].events = POLLOUT;
                continue;
            }
            this->pollfds[i].events = POLLIN;
            if (this->clients[i].write_buffer.size() > 0) {
                this->pollfds[i].events |= POLLOUT;
            }
            if (this->clients[i].place != -1) {
                this->players[this->clients[i].place].id = i;
            } 
        }
        //Accept new clients
        if (this->pollfds[0].revents & POLLIN) {
            sockaddr_in6 client_address;
            socklen_t client_address_len = sizeof client_address;
            int client_fd = accept(this->pollfds[0].fd, (struct sockaddr *) &client_address, &client_address_len);
            if (client_fd < 0) continue;
            fcntl(client_fd, F_SETFL, O_NONBLOCK);
            this->pollfds.push_back({client_fd, POLLIN, 0});
            ClientInfo client_info;
            char buffer[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, &client_address.sin6_addr, buffer, INET6_ADDRSTRLEN) == nullptr) {
                syserr("inet_ntop");
            }
            client_info.ip = buffer;
            client_info.port = ntohs(client_address.sin6_port);
            this->clients.push_back(client_info);
        }
        this->pollfds[0].revents = 0;
        this->pollfds[0].events = POLLIN;
        if (this->game_over) break;
    }
    while (clients.size() > 1) {
        for (int i = 1; i < (int)this->pollfds.size(); i++) this->pollfds[i].revents = 0;
        int poll_status = poll(this->pollfds.data(), this->pollfds.size(), this->timeout);
        if (poll_status < 0) syserr("poll");
        for (int i = 1; i < (int)this->pollfds.size(); i++) {
            if (this->clients[i].write_buffer.size() == 0) {
                this->clients.erase(this->clients.begin() + i);
                this->pollfds.erase(this->pollfds.begin() + i);
                --i;    
            }
            if (this->pollfds[i].revents & POLLOUT) {
                ssize_t message_length = write(this->pollfds[i].fd, this->clients[i].write_buffer.c_str(), this->clients[i].write_buffer.size());
                if (message_length < 0) continue;
                this->clients[i].write_buffer = this->clients[i].write_buffer.substr(message_length);
            }
            if (this->clients[i].write_buffer.size() > 0) this->pollfds[i].events = POLLOUT;
        }
    }
    close(this->pollfds[0].fd);
}

void Game::create_server_socket(uint16_t port) {
    int socket_fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0) syserr("cannot create a socket");

    sockaddr_in6 server_address = {};
    server_address.sin6_family = AF_INET6;
    server_address.sin6_addr = in6addr_any;
    server_address.sin6_port = htons(port);

    if (bind(socket_fd, (struct sockaddr *) &server_address, (socklen_t) sizeof server_address) < 0) {
        syserr("bind");
    }

    if (listen(socket_fd, QUEUE_LENGTH) < 0) {
        syserr("listen");
    }

    fcntl(socket_fd, F_SETFL, O_NONBLOCK);
    this->pollfds.push_back({socket_fd, POLLIN, 0});

    ClientInfo server_info;
    socklen_t lenght = (socklen_t) sizeof server_address;
    if (getsockname(socket_fd, (struct sockaddr *) &server_address, &lenght) < 0) {
        syserr("getsockname");
    }
    server_info.port = ntohs(server_address.sin6_port);

    char buffer[INET6_ADDRSTRLEN];
    if (inet_ntop(AF_INET6, &server_address.sin6_addr, buffer, INET6_ADDRSTRLEN) == nullptr) {
        syserr("inet_ntop");
    }
    server_info.ip = buffer;

    this->clients.push_back(server_info);
}

void Game::handle_messages() {
    //Recieve messages
    for (int i = 1; i < (int)this->pollfds.size(); i++) {
        if (this->pollfds[i].fd == -1 || this->clients[i].disconnect) continue;
        string message = extract_message(this->clients[i].read_buffer);
        while (message != "") {
            cout << "[" + clients[i].ip + ":" + to_string(clients[i].port) + "," + clients[0].ip + ":" + to_string(clients[0].port) + ',' + get_timestamp() + "] " + message + "\r\n"; 
            if (this->clients[i].place == -1) {
                if (this->is_iam(message)) {
                    int place = place_number(message[3]);
                    if (this->players[place].id == 0) {
                        this->clients[i].place = place;
                        this->players[place].id = i;
                        this->connected_clients++;
                        if (this->phase == 1) this->reconnect_player(place);
                    }
                    else {
                        string response = "BUSY";
                        for (int i = 0; i < 4; ++i) {
                            if (this->players[i].id != 0) response += "NESW"[i];
                        }
                        response += "\r\n";
                        this->send_message(i, response);
                        this->clients[i].disconnect = true;
                    }
                }
                else {
                    close(this->pollfds[i].fd);
                    this->pollfds[i].fd = -1;
                }
            }
            else {
                pair<int, string> trick_value = this->is_trick(message);
                if (trick_value.first != -1) {
                    if (this->check_trick(trick_value.second, clients[i].place)) {
                        this->trick_cards.push_back(string_to_card(trick_value.second));
                        this->players[this->current_player].remove_card(string_to_card(trick_value.second));
                        this->current_player = (this->current_player + 1) % 4;
                        this->timeout_passed = true;
                    }
                    else {
                        string response = "WRONG" + to_string(this->trick_number) + "\r\n";
                        this->send_message(i, response);
                    }
                }
                else {
                    close(this->pollfds[i].fd);
                    this->pollfds[i].fd = -1;
                }
            }
            message = extract_message(this->clients[i].read_buffer);
        }   
    }
    //Send messages
    if (this->connected_clients != 4) return;
    if (this->phase == 0) {
        //Send DEAL
        Round r = this->rounds[this->round];
        for (int i = 0; i < 4; i++) {
            string message = "DEAL" + to_string(r.type) + r.starting_player + r.player_cards_string[i] + "\r\n";
            this->players[i].give_cards(r.player_cards[i]);
            this->send_message(this->players[i].id, message);
        }
        this->phase = 1;
        this->trick_number = 1;
        this->current_player = place_number(r.starting_player);
        this->timeout_passed = true;
    }
    if (this->phase == 1) {
        //Send TRICK or TAKEN
        if(this->trick_cards.size() == 4) this->send_taken();
        if (this->trick_cards.size() != 4 && this->timeout_passed) this->send_trick();
    }
    if (this->phase == 2) {
        //Send SCORE and TOTAL
        this->send_score_and_total();
    }
}

bool Game::is_iam(string message) {
    regex pattern(R"(IAM(N|E|W|S))");
    return regex_match(message, pattern);
}

pair<int, string> Game::is_trick(string message) {
    regex pattern(R"(TRICK(1[0-3]|[1-9])(10|[2-9]|J|Q|K|A)(C|D|H|S))");
    smatch matches;
    if (regex_match(message, matches, pattern)) {
        int number = stoi(matches[1].str());
        string card = matches[2].str();
        card += matches[3].str();
        return {number, card};
    }
    return {-1, ""};
}

void Game::send_trick() {
    if (this->players[this->current_player].id == 0) return;
    this->timeout_passed = false;
    string message = "TRICK" + to_string(this->trick_number);
    for (int i = 0; i < (int)this->trick_cards.size(); ++i) {
        message += card_to_string(this->trick_cards[i]);
    }
    message += "\r\n";
    this->send_message(this->players[this->current_player].id, message);
}

void Game::send_taken() {
    int winner_id = 0;
    int max_value = 0;
    for (int i = 0; i < 4; ++i) {
        if (this->trick_cards[i].color == this->trick_cards[0].color && this->trick_cards[i].value > max_value) {
            max_value = this->trick_cards[i].value;
            winner_id = i;
        }
    }
    char winner = "NESW"[(this->current_player + winner_id) % 4];
    this->count_points(winner, this->rounds[this->round].type);

    string message = "TAKEN" + to_string(this->trick_number);
    for (int i = 0; i < (int)this->trick_cards.size(); ++i) {
        message += card_to_string(this->trick_cards[i]);
    }
    message += winner;
    message += "\r\n";
    for (int i = 0; i < 4; ++i) {
        this->send_message(this->players[i].id, message);
    }
    log.push_back(message);
    this->trick_cards.clear();
    this->trick_number++;
    this->timeout_passed = true;
    this->current_player = place_number(winner);
    if (this->trick_number == 14) {
        this->timeout_passed = false;
        this->phase = 2;
        log.clear();
    }
}

void Game::send_score_and_total() {
    string message = "SCORE";
    for (int i = 0; i < 4; ++i) {
        message += "NESW"[i];
        message += to_string(this->players[i].round_points);
    }
    message += "\r\n";
    for (int i = 0; i < 4; ++i) {
        this->send_message(this->players[i].id, message);
    }
    for (int i = 0; i < 4; ++i) {
        this->players[i].total_points += this->players[i].round_points;
        this->players[i].round_points = 0;
    }
    message = "TOTAL";
    for (int i = 0; i < 4; ++i) {
        message += "NESW"[i];
        message += to_string(this->players[i].total_points);
    }
    message += "\r\n";
    for (int i = 0; i < 4; ++i) {
        this->send_message(this->players[i].id, message);
    }
    this->round++;
    this->phase = 0;
    this->log.clear();
    if (this->round == (int)this->rounds.size()) {
        this->game_over = true;
    }
}

void Game::reconnect_player(int place) {
    Round r = this->rounds[this->round];
    string message = "DEAL" + to_string(r.type) + r.starting_player + r.player_cards_string[place] + "\r\n";
    this->send_message(this->players[place].id, message);
    for (string l : this->log) {
        this->send_message(this->players[place].id, l);
    }
    if (this->phase == 1 && this->current_player == place) this->timeout_passed = true;
}

bool Game::check_trick(string message, int id) {
    Card card = string_to_card(message);
    if (this->phase != 1) return false;
    if (this->current_player != id) return false;
    if (this->trick_cards.size() == 4) return false;
    if (!this->players[id].has_card(card)) return false;
    if (this->trick_cards.size() == 0) return true;
    if (this->trick_cards[0].color != card.color && this->players[id].has_color(this->trick_cards[0].color)) return false;
    return true;
}

void Game::count_points(char winner, int round_type) {
    int points = 0;
    if (round_type == 1 || round_type == 7) points++;
    if (round_type == 2 || round_type == 7) {
        for (int i = 0; i < (int)this->trick_cards.size(); ++i) {
            if (this->trick_cards[i].color == 'H') points++;
        }
    }
    if (round_type == 3 || round_type == 7) {
        for (int i = 0; i < (int)this->trick_cards.size(); ++i) {
            if (this->trick_cards[i].value == 12) points += 5;
        }
    }
    if (round_type == 4 || round_type == 7) {
        for (int i = 0; i < (int)this->trick_cards.size(); ++i) {
            if (this->trick_cards[i].value == 11 || this->trick_cards[i].value == 13) points += 2;
        }
    }
    if (round_type == 5 || round_type == 7) {
        for (int i = 0; i < (int)this->trick_cards.size(); ++i) {
            if (this->trick_cards[i].value == 13 && this->trick_cards[i].color == 'H') points += 18;
        }
    }
    if (round_type == 6 || round_type == 7) {
        if (this->trick_number == 7 || this->trick_number == 13) points += 10;
    }
    this->players[place_number(winner)].round_points += points;
}

void Game::send_message(int id, string message) {
    this->clients[id].write_buffer += message;
    cout << "[" + clients[0].ip + ":" + to_string(clients[0].port) + "," + clients[id].ip + ":" + to_string(clients[id].port) + ',' + get_timestamp() + "] " + message; 
}

int main(int argc, char *argv[]) {
    uint16_t port = 0;
    string file = "";
    int timeout = 5;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-p") {
            if (i + 1 >= argc) fatal("Missing argument for -p");
            else port = read_port(argv[++i]);
        }
        else if (arg == "-f") {
            if (i + 1 >= argc) fatal("Missing argument for -f");
            else file = argv[++i];
        }
        else if (arg == "-t") {
            if (i + 1 >= argc) fatal("Missing argument for -t");
            else timeout = stoi(argv[++i]);
        }
        else fatal("Incorrect arguements");
    }
    if (file == "") fatal("Missing file name");

    Game game(port, file, timeout * 1000);
    game.run();
    return 0;
}