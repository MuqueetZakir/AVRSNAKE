#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <EEPROM.h>

#include "lib/meta.h"
#include "lib/arduino_pins.h"
#include "lib/calc.h"
#include "lib/adc.h"
#include "lib/lcd.h"
#include "lib/debounce.h"

#define LCD_PIN_RS D10
#define LCD_PIN_RW D11
#define LCD_PIN_E  D12
#define LCD_PIN_D4 D13
#define LCD_PIN_D5 D14
#define LCD_PIN_D6 D15
#define LCD_PIN_D7 D16

#define BTN_LEFT     D2
#define BTN_RIGHT    D3
#define BTN_UP       D4
#define BTN_DOWN     D5
#define BTN_PAUSE    D6
#define BTN_RESTART  D7

#define D_LEFT      0
#define D_RIGHT     1
#define D_UP        2
#define D_DOWN      3
#define D_PAUSE     4
#define D_RESTART   5

#define DEBO_CHANNELS 6
#define DEBO_TICKS 1  // in 0.01s

#define ROWS 4
#define COLS 20
#define STEP_DELAY 24

#define BUZZER_PIN 9  // Sound effects

// Sub-glyphs
#define _HEAD_ 15, 21, 21, 30
#define _BODY_ 15, 31, 31, 30
#define _FOOD_ 10, 21, 17, 14
#define _NONE_  0,  0,  0,  0

const uint8_t SYMBOL_BB[] PROGMEM = {_BODY_, _BODY_};
const uint8_t SYMBOL_BX[] PROGMEM = {_BODY_, _NONE_};
const uint8_t SYMBOL_XB[] PROGMEM = {_NONE_, _BODY_};
const uint8_t SYMBOL_HX[] PROGMEM = {_HEAD_, _NONE_};
const uint8_t SYMBOL_XH[] PROGMEM = {_NONE_, _HEAD_};
const uint8_t SYMBOL_BH[] PROGMEM = {_BODY_, _HEAD_};
const uint8_t SYMBOL_HB[] PROGMEM = {_HEAD_, _BODY_};
const uint8_t SYMBOL_HF[] PROGMEM = {_HEAD_, _FOOD_};
const uint8_t SYMBOL_FH[] PROGMEM = {_FOOD_, _HEAD_};
const uint8_t SYMBOL_FX[] PROGMEM = {_FOOD_, _NONE_};
const uint8_t SYMBOL_XF[] PROGMEM = {_NONE_, _FOOD_};
const uint8_t SYMBOL_BF[] PROGMEM = {_BODY_, _FOOD_};
const uint8_t SYMBOL_FB[] PROGMEM = {_FOOD_, _BODY_};

typedef enum {
    bEMPTY = 0x00,
    bHEAD = 0x01,
    bFOOD = 0x02,
    bBODY_LEFT = 0x80,
    bBODY_RIGHT = 0x81,
    bBODY_UP = 0x82,
    bBODY_DOWN = 0x83,
    bOBSTACLE = 0x04
} block_t;

typedef enum {
    dLEFT = 0x00,
    dRIGHT = 0x01,
    dUP = 0x02,
    dDOWN = 0x03,
} dir_t;

typedef struct {
    int8_t x;
    int8_t y;
} coord_t;

bool crashed;
uint8_t snake_len;
uint16_t score = 0;    // Added score tracking
uint16_t highScore = 0; // EEPROM stored high score

block_t board[ROWS][COLS];
coord_t head_pos;
coord_t tail_pos;
dir_t head_dir;

bool restart_held;
bool pause_held;
bool paused;
uint8_t presc = 0;

void init_cgram() {
    lcd_define_glyph_pgm(0, SYMBOL_BB);
    lcd_define_glyph_pgm(1, SYMBOL_BX);
    lcd_define_glyph_pgm(2, SYMBOL_XB);
    lcd_define_glyph_pgm(3, SYMBOL_HX);
    lcd_define_glyph_pgm(4, SYMBOL_FX);
    lcd_define_glyph_pgm(5, SYMBOL_XF);
}

void playEatSound() {
    tone(BUZZER_PIN, 1000, 100);  // 1000 Hz for 100 ms
}

void playCrashSound() {
    tone(BUZZER_PIN, 500, 500);  // 500 Hz for 500 ms
}

void updateScore() {
    score += 10;  // Increase by 10 for each food
    lcd_set_cursor(0, 3);  // Show score on last row
    lcd_print("Score: ");
    lcd_print(score);
}

void loadHighScore() {
    EEPROM.get(0, highScore);  // Read high score from EEPROM
}

void saveHighScore() {
    if (score > highScore) {
        highScore = score;
        EEPROM.put(0, highScore);  // Save new high score
    }
}

void displayHighScore() {
    lcd_set_cursor(10, 3);  // Show high score next to current score
    lcd_print("Hi: ");
    lcd_print(highScore);
}

void gameOver() {
    playCrashSound();
    lcd_clear();
    lcd_set_cursor(5, 1);
    lcd_print("Game Over");
    lcd_set_cursor(4, 2);
    lcd_print("Score: ");
    lcd_print(score);
    saveHighScore();  // Save the high score if necessary
    displayHighScore();
    _delay_ms(2000);  // Delay to display message
    init_gameboard();  // Restart game after game over
}

void snakeGrowAnimation() {
    for (uint8_t i = 0; i < 3; i++) {
        lcd_write_custom_char(0);  // Blink snake head or change color
        _delay_ms(100);
        lcd_clear();
        _delay_ms(100);
    }
}

void adjustSpeed() {
    if (score % 50 == 0 && STEP_DELAY > 10) {  // Increase speed every 50 points
        STEP_DELAY--;
    }
}

void place_food() {
    while (1) {
        uint8_t xx = rand() % COLS;
        uint8_t yy = rand() % ROWS;

        if (board[yy][xx] == bEMPTY) {
            board[yy][xx] = bFOOD;
            break;
        }
    }
}

void placeObstacles(uint8_t numObstacles) {
    for (uint8_t i = 0; i < numObstacles; i++) {
        uint8_t x = rand() % COLS;
        uint8_t y = rand() % ROWS;
        if (board[y][x] == bEMPTY) {
            board[y][x] = bOBSTACLE;  // Obstacles represented by '4'
        }
    }
}

void init_gameboard() {
    for (uint8_t x = 0; x < COLS; x++) {
        for (uint8_t y = 0; y < ROWS; y++) {
            board[y][x] = bEMPTY;
        }
    }

    lcd_clear();
    tail_pos = (coord_t){.x = 0, .y = 0};
    board[0][0] = bBODY_RIGHT;
    board[0][1] = bBODY_RIGHT;
    board[0][2] = bBODY_RIGHT;
    board[0][3] = bHEAD;
    head_pos = (coord_t){.x = 3, .y = 0};
    snake_len = 4;
    head_dir = dRIGHT;
    crashed = false;
    place_food();
    placeObstacles(5);  // Add 5 obstacles to the board
}

void renderPauseScreen() {
    lcd_clear();
    lcd_set_cursor(5, 1);
    lcd_print("Paused");
    _delay_ms(500);
}

void update() {
    if (debo_get_pin(D_RESTART)) {
        if (!restart_held) {
            init_gameboard();
            presc = 0;
            restart_held = true;
        }
    } else {
        restart_held = false;
    }

    if (debo_get_pin(D_PAUSE)) {
        if (!pause_held) {
            paused ^= true;
            pause_held = true;
        }
    } else {
        pause_held = false;
    }

    if (paused) {
        renderPauseScreen();
        return;
    }

    if (!crashed) {
        if (debo_get_pin(D_LEFT)) head_dir = dLEFT;
        else if (debo_get_pin(D_RIGHT)) head_dir = dRIGHT;
        else if (debo_get_pin(D_UP)) head_dir = dUP;
        else if (debo_get_pin(D_DOWN)) head_dir = dDOWN;

        if (presc++ == STEP_DELAY) {
            presc = 0;

            coord_t oldpos = head_pos;
            switch (head_dir) {
                case dLEFT:  head_pos.x--; break;
                case dRIGHT: head_pos.x++; break;
                case dUP:    head_pos.y--; break;
                case dDOWN:  head_pos.y++; break;
            }

            if (head_pos.x == -1 || head_pos.x == COLS ||
                head_pos.y == -1 || head_pos.y == ROWS ||
                board[head_pos.y][head_pos.x] != bEMPTY) {
                crashed = true;
                gameOver();  // Game over on crash
            } else {
                if (board[head_pos.y][head_pos.x] == bFOOD) {
                    board[oldpos.y][oldpos.x] = bBODY_LEFT + head_dir;
                    board[head_pos.y][head_pos.x] = bHEAD;
                    snake_len++;
                    place_food();
                    playEatSound();  // Play sound when food is eaten
                    snakeGrowAnimation();  // Growth animation
                    updateScore();  // Update score
                    adjustSpeed();  // Increase speed
                } else {
                    board[oldpos.y][oldpos.x] = bBODY_LEFT + head_dir;
                    coord_t new_tail = tail_pos;
                    switch (board[new_tail.y][new_tail.x]) {
                        case bBODY_LEFT:  new_tail.x--; break;
                        case bBODY_RIGHT: new_tail.x++; break;
                        case bBODY_UP:    new_tail.y--; break;
                        case bBODY_DOWN:  new_tail.y++; break;
                    }
                    board[tail_pos.y][tail_pos.x] = bEMPTY;
                    tail_pos = new_tail;
                }
            }
        }
    }
}

int main(void) {
    lcd_init(LCD_PIN_RS, LCD_PIN_RW, LCD_PIN_E, LCD_PIN_D4, LCD_PIN_D5, LCD_PIN_D6, LCD_PIN_D7);
    lcd_clear();
    lcd_print("Snake game");

    init_debo(DEBO_CHANNELS, DEBO_TICKS);
    debo_enable_pin(D_LEFT, BTN_LEFT);
    debo_enable_pin(D_RIGHT, BTN_RIGHT);
    debo_enable_pin(D_UP, BTN_UP);
    debo_enable_pin(D_DOWN, BTN_DOWN);
    debo_enable_pin(D_PAUSE, BTN_PAUSE);
    debo_enable_pin(D_RESTART, BTN_RESTART);

    lcd_set_cursor(0, 1);
    lcd_print("Press any key");

    init_gameboard();  // Initialize game
    loadHighScore();   // Load high score from EEPROM

    while (1) {
        update();
    }
}
