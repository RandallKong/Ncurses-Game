#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>

// Function to set terminal to raw mode
void enableRawMode() {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &raw);
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Function to read a keypress
char readKey() {
    char c;
    read(STDIN_FILENO, &c, 1);
    return c;
}

int main() {
    enableRawMode(); // Set terminal to raw mode

    char c;
    while (1) {
        c = readKey();
        if (c == '\x1b') { // Check if the first byte is the escape character
            char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) break;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) break;

            if (seq[0] == '[') {
                switch(seq[1]) {
                    case 'A':
                        printf("Up Arrow Key pressed\n");
                        break;
                    case 'B':
                        printf("Down Arrow Key pressed\n");
                        break;
                    case 'C':
                        printf("Right Arrow Key pressed\n");
                        break;
                    case 'D':
                        printf("Left Arrow Key pressed\n");
                        break;
                }
            }
        }
        else {
            printf("Key pressed: %c\n", c);
        }
    }

    return 0;
}
