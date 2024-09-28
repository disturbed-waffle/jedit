#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <termio.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#define VERSION "0.0.1"
#define TAB_STOP 8

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
    int rsize;
    char *chars;
    char *render;
}Erow;

struct EditorConfig {
    int cx, cy;
    int rx;
    int row_off;
    int col_off;
    int screen_rows;
    int screen_cols;
    int num_rows;
    Erow *row;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
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

//---row opperations---
int editor_row_cx_to_rx(Erow *row, int cx){
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++){
        if (row->chars[j] == '\t')
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        rx++;
    }
    return rx;
}

void editor_update_row(Erow *row){
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++){
        if (row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(TAB_STOP-1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++){
        if (row->chars[j] == '\t'){
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
        }else{
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editor_append_row(char *s, size_t len){
    E.row = realloc(E.row, sizeof(Erow) * (E.num_rows + 1));

    int at = E.num_rows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editor_update_row(&E.row[at]);

    E.num_rows++;
}

//---file i/o---

void editor_open(char *file_name){
    free(E.filename);
    E.filename = strdup(file_name);
    FILE *fp = fopen(file_name, "r");
    if (!fp) die("fopen"); // fp is NULL

    char *line = NULL;
    size_t linecap = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &linecap, fp)) != -1){
        while (line_len > 0 && (line[line_len - 1] == '\n' 
                || line[line_len - 1] == '\r'))
            line_len--;

        editor_append_row(line, line_len);
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
void editor_scroll(){
    E.rx = 0;
    if (E.cy < E.num_rows){
        E.rx = editor_row_cx_to_rx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.row_off){
        E.row_off = E.cy;
    }
    if (E.cy >= E.row_off + E.screen_rows){
        E.row_off = E.cy - E.screen_rows + 1;
    }

    if (E.rx < E.col_off) {
        E.col_off = E.rx;
    }
    if (E.rx > E.col_off + E.screen_cols){
        E.col_off = E.rx - E.screen_cols + 1;
    }
}

void editor_draw_rows(Abuf *ab){
    int y;
    for (y = 0; y < E.screen_rows; y++){
        int file_row = y + E.row_off;
        if (file_row >= E.num_rows){
            
            // Show version info
            if (E.num_rows == 0 && y == E.screen_rows / 3){
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                "JTEXT EDITOR -- VERSION %s", VERSION);

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
            int len = E.row[file_row].rsize - E.col_off;
            if (len < 0) len = 0;
            if (len > E.screen_cols) len = E.screen_cols;
            ab_append(ab, &E.row[file_row].render[E.col_off], len);
        }

        ab_append(ab, "\x1b[K", 3);
        ab_append(ab, "\r\n", 2);
        
    }
}

void editor_draw_status_bar(Abuf *ab){
    ab_append(ab, "\x1b[7m", 4); // Inverted colors
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
        E.filename ? E.filename : "[No Name]", E.num_rows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.num_rows);
    if (len > E.screen_cols) len = E.screen_cols;
    ab_append(ab, status, len);
    while (len < E.screen_cols){
        if (E.screen_cols - len == rlen){
            ab_append(ab, rstatus, rlen);
            break;
        }else{
            ab_append(ab, " ", 1);
            len++;
        }
    }
    ab_append(ab, "\x1b[m", 3); // reset inverted colors
    ab_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(Abuf *ab){
    ab_append(ab, "\x1b[K", 3);
    int msg_len = strlen(E.statusmsg);
    if (msg_len > E.screen_cols) msg_len = E.screen_cols;
    if (msg_len && time(NULL) - E.statusmsg_time < 5)
        ab_append(ab, E.statusmsg, msg_len);
}

void editor_refresh_screen(){
    editor_scroll();

    Abuf ab = ABUF_INIT;

    ab_append(&ab, "\x1b[?25l", 6); // Hide cursor
    ab_append(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", 
            (E.cy - E.row_off) + 1, (E.rx - E.col_off) + 1);
    ab_append(&ab, buf, strlen(buf));
    ab_append(&ab, "\x1b[?25h", 6); // Show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

void editor_set_status_message(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

//---input----
void editor_move_cursor(int key){
    Erow *row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];

    switch (key){
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            }else if (E.cy > 0){
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size){
                E.cx++;
            }else if (row && E.cx == row->size){
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0){
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.num_rows){
                E.cy++;
            }
            break;
    }

    row = (E.cy > E.num_rows) ? NULL : &E.row[E.cy];
    int row_len = row ? row->size : 0;
    if (E.cx > row_len){
        E.cx = row_len;
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
               if (c == PAGE_UP){
                E.cy = E.row_off;
               }else if (c == PAGE_DOWN){
                E.cy = E.row_off + E.screen_rows - 1;
                if (E.cy > E.num_rows) E.cy = E.num_rows;
               }

               int times = E.screen_rows;
               while (times--){
                editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
               }
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
    E.rx = 0;
    E.row_off = 0;
    E.num_rows = 0;
    E.row = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1){
        die("get_window_size");
    }
    E.screen_rows -= 2;
}

int main(int argc, char *argv[]){
    enable_raw_mode();
    init_editor();
    if (argc >= 2){
        editor_open(argv[1]);
    }

    editor_set_status_message("HELP: Ctrl-Q = quit");

    while (1){
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}