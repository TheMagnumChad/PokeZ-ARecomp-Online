#ifndef ZA_BATTLE_KEYS_H
#define ZA_BATTLE_KEYS_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { float x,y; int key; bool pressed; } ZaBattleKey;
#ifdef __cplusplus
extern "C" {
#endif
bool input_keyboard_attack_keys(ZaBattleKey keys[4]);
#ifdef __cplusplus
}
#endif
#endif
