#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <inttypes.h>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>

#include "err.h"
#include "common.h"

using namespace std;

bool is_color(char c) {
    return c == 'C' || c == 'D' || c == 'H' || c == 'S';
}

string extract_message(string &buffer) {
    string message = "";
    int p = 0;
    while (p < (int)buffer.size()) {
        if (p + 1 == (int)buffer.size()) break;
        if (buffer[p] == '\r' && buffer[p + 1] == '\n') {
            message = buffer.substr(0, p);
            buffer = buffer.substr(p + 2);
            break;
        }
        p++;
    }
    return message;
}

string extract_stdin_message(string &buffer) {
    string message = "";
    int p = 0;
    while (p < (int)buffer.size()) {
        if (buffer[p] == '\n') {
            message = buffer.substr(0, p);
            buffer = buffer.substr(p + 1);
            break;
        }
        p++;
    }
    return message;
}

Card string_to_card(string input) {
    Card result;
    if (input.size() == 3) {
        result.value = 10;
        result.color = input[2];
    }
    else {
        if (input[0] == 'J') result.value = 11;
        else if (input[0] == 'Q') result.value = 12;
        else if (input[0] == 'K') result.value = 13;
        else if (input[0] == 'A') result.value = 14;
        else result.value = input[0] - '0';
        result.color = input[1];    
    }
    return result;
}

string card_to_string(Card card) {
    string result = "";
    if (card.value <= 10) result = to_string(card.value);
    else if (card.value == 11) result = "J";
    else if (card.value == 12) result = "Q";
    else if (card.value == 13) result = "K";
    else result = "A";
    result += card.color;
    return result;
}

vector<Card> string_to_card_vector(string input) {
    int p = 0;
    vector<Card> cards;
    while (p < (int)input.size()) {
        string card_string = "";
        card_string += input[p];
        if (input[p+1] == '0') {
            p++;
            card_string += input[p];
        }
        p++;
        card_string += input[p];
        p++;
        cards.push_back(string_to_card(card_string));
    }
    return cards;
}

int place_number(char player) {
    if (player == 'N') return 0;
    if (player == 'E') return 1;
    if (player == 'S') return 2;
    if (player == 'W') return 3;
    return -1;
}

uint16_t read_port(char const *string) {
    char *endptr;
    unsigned long port = strtoul(string, &endptr, 10);
    if ((port == ULONG_MAX && errno == ERANGE) || *endptr != 0 || port == 0 || port > UINT16_MAX) {
        fatal("%s is not a valid port number", string);
    }
    return (uint16_t) port;
}

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    auto time_t_now = std::chrono::system_clock::to_time_t(now);

    std::tm tm_now;
    localtime_r(&time_t_now, &tm_now);

    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << milliseconds.count();

    return oss.str();
}