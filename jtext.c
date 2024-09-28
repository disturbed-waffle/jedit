#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <termio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum EditorKey{
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

typedef struct Erow{
    int size;
    char *chars;
}Erow;

struct EditorConfig {
    int cx, cy;
    int screen_rows;
    int screen_cols;
    int num_rows;
    Erow row;
    struct termios orig_termios;
};

struct EditorConfig E;

//---terminal---
void die(const char *s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disable_raw_mode(){
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1){
        die("tcsetattr");
    }
}

void enable_raw_mode(){
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1){
        die("tcgetattr");
    }
    atexit(disable_raw_mode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1){
        die("tcsetattr");
    }
}

int editor_read_key(){
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if (nread == -1 && errno != EAGAIN){
            die("read");
        }
    }

    if (c == '\x1b'){
        char seq[3];

        if (read(STDIN_FILENO, seq, 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, seq + 1, 1) != 1) return '\x1b';

        if (seq[0] == '['){
            if (seq[1] >= '0' && seq[1] <= '9'){
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~'){
                    switch (seq[1]){
                        case '3': return DEL_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                    }
                }
            }else{
                switch(seq[1]){
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                }
            }
        }

        return '\x1b';
    }else{
        return c; 
    }
}

int get_cursor_position(int *rows, int *cols){
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) -1){
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return 1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int get_window_size(int *rows, int *cols){
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return get_cursor_position(rows, cols);
    }else{
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

//---file i/o---

void editor_open(char *file_name){
    FILE *fp = fopen(file_name, "r");
    if (!fp) die("fopen"); // fp is NULL

    char *line = NULL;
    size_t linecap = 0;
    ssize_t line_len;
    line_len = getline(&line, &linecap, fp);
    if (line_len != -1){
        while (line_len > 0 && (line[line_len - 1] == '\n' 
                || line[line_len - 1] == '\r'))
            line_len--;


        E.row.size = line_len;
        E.row.chars = malloc(line_len + 1);
        memcpy(E.row.chars, line, line_len);
        E.row.chars[line_len] = '\0';
        E.num_rows = 1;
    }
    free(line);
    fclose(fp);

}

//---append buffer---

typedef struct{
    char *b;
    int len;
}Abuf;

#define ABUF_INIT {NULL, 0}

void ab_append(Abuf *ab, const char *s, int len){
    char *new = realloc(ab->b, ab->len + len);
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void ab_free(Abuf *ab){
    free(ab->b);
}

//---output---
void editor_draw_rows(Abuf *ab){
    int y;
    for (y = 0; y < E.screen_rows; y++){
        if (y >= E.num_rows){
            // Show version info
            if (y == E.screen_rows / 3){
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                "JText editor -- version %s", VERSION);

                if (welcomelen > E.screen_cols) welcomelen = E.screen_cols;
                int padding = (E.screen_cols - welcomelen) / 2;
                if (padding != 0){
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while (padding-- != 0) {
                    ab_append(ab, " ", 1);
                }

                ab_append(ab, welcome, welcomelen);
            }else{
                ab_append(ab, "~", 1);
            }
        }else{
            int len = E.row.size;
            if (len > E.screen_cols) len = E.screen_cols;
            ab_append(ab, E.row.chars, len);
        }

        ab_append(ab, "\x1b[K", 3);
        if (y < E.screen_rows - 1){
            ab_append(ab, "\r\n", 2);
        }
    }
}

void editor_refresh_screen(){
    Abuf ab = ABUF_INIT;

    ab_append(&ab, "\x1b[?25l", 6); // Hide cursor
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    ab_append(&ab, buf, strlen(buf));
    ab_append(&ab, "\x1b[?25h", 6); // Show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

//---input----
void editor_move_cursor(int key){
    switch (key){
        case ARROW_LEFT:
            if (E.cx != 0) E.cx--;
            break;
        case ARROW_RIGHT:
            if (E.cx != E.screen_cols - 1) E.cx++;
            break;
        case ARROW_UP:
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy != E.screen_rows - 1) E.cy++;
            break;
    }
}

void editor_process_keypress(){
    int c = editor_read_key();

    switch(c){
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screen_rows;
                while (times--)
                    editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editor_move_cursor(c);
            break;
    }
}

//---init---
void init_editor(){
    E.cx = 0;
    E.cy = 0;
    E.num_rows = 0;
    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1){
        die("get_window_size");
    }
}

int main(int argc, char *argv[]){
    enable_raw_mode();
    init_editor();
    if (argc >= 2){
        editor_open(argv[1]);
    }

    while (1){
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}