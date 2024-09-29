#include <vector>

#ifndef MIM_COMMON_H
#define MIM_COMMON_H

struct Card {
    char color;
    int value;
};

bool is_color(char c);
std::string extract_message(std::string &buffer);
std::string extract_stdin_message(std::string &buffer);
Card string_to_card(std::string input);
int place_number(char player);
std::string card_to_string(Card card);
std::vector<Card> string_to_card_vector(std::string input);
uint16_t read_port(char const *string);
std::string get_timestamp();

#endif