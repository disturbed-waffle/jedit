#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <termio.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#define VERSION "0.0.1"
#define TAB_STOP 4
#define QUIT_TIMES 1

#define CTRL_KEY(k) ((k) & 0x1f)

enum EditorKey{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    PAGE_UP,
    PAGE_DOWN,
};

enum EditorHighlight{
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
	HL_KEYWORD1,
	HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0) // 01
#define HL_HIGHLIGHT_STRINGS (1<<1) // 10

typedef struct EditorSyntax{
    char *filetype;
    char **filematch;
    char **keywords;
    char *single_line_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
} EditorSyntax;

typedef struct Erow{
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_open_comment;
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
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    EditorSyntax *syntax;
    struct termios orig_termios;
};

struct EditorConfig E;

//---filetypes---
char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL
};

char *PY_HL_extensions[] = {".py", NULL};
char *PY_HL_keywords[] = {
    "None", "break", "except", "in", "raise", "False", "await", "else", "import",
    "pass", "and", "continue", "for", "lambda", "try", "True", "class", "finally",
    "is", "return", "as", "def", "from", "nonlocal", "while", "async", "elif", "if",
    "not", "with", "assert", "del", "global", "or", "yield",
    "str|", "int|", "float|", "complex|", "list|", "tuple|", "range|", "dict|", "set|",
    "frozenset|", "bool|", "bytes|", "bytearray|", "memoryview|", "NoneType|"
}; // Wtf are some of these

EditorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
    {
        "python",
        PY_HL_extensions,
        PY_HL_keywords,
        "#", NULL, NULL,
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

//---Prototypes---
void editor_set_status_message(const char *fmt, ...);
void editor_refresh_screen();
char *editor_prompt(char *prompt, void (*callback)(char *, int));

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

//---syntax highlighting---
int is_separator(int c){
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editor_update_syntax(Erow *row){
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if (E.syntax == NULL) return;

    char **keywords = E.syntax->keywords;

    char *scs = E.syntax->single_line_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while (i < row->rsize){
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        // Comments
        if (scs_len && !in_string && !in_comment){
            if (!strncmp(row->render + i, scs, scs_len)){
                memset(row->hl + i, HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if (mcs_len && mce_len && !in_string){
            if (in_comment){
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)){
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else{
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)){
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }


        // Strings
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS){
            if (in_string){
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize){
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            }else{
                if (c == '"' || c == '\''){
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }
        // Numbers
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS){
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)){
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep){
            int j;
            for (j = 0; keywords[j] != NULL; j++){
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;

                if (!strncmp(&row->render[i], keywords[j], klen) && 
                    is_separator(row->render[i + klen])) {

                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL){
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.num_rows)
        editor_update_syntax(&E.row[row->idx + 1]); 
}

int editor_syntax_to_color(int hl){
    switch (hl){
        case HL_MLCOMMENT:
        case HL_COMMENT: return 36;
		case HL_KEYWORD1: return 33;
		case HL_KEYWORD2: return 32;       
		case HL_STRING: return 35;
        case HL_NUMBER: return 31;
        case HL_MATCH: return 34;
        default: return 37;
    }
}

void editor_select_syntax_highlight(){
    E.syntax = NULL;
    if (E.filename == NULL) return;

    char *ext = strrchr(E.filename, '.');
    unsigned int j;
    for (j = 0; j < HLDB_ENTRIES; j++){
        EditorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]){
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;     

                int file_row;
                for (file_row = 0; file_row < E.num_rows; file_row++){
                    editor_update_syntax(&E.row[file_row]);
                }

                return;
            }
            i++;
        }
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

int editor_row_rx_to_cx(Erow *row, int rx){
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++){
        if (row->chars[cx] == '\t')
            cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP);
        cur_rx++;

        if (cur_rx > rx) return cx;
    }
    return cx;
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

    editor_update_syntax(row);
}

void editor_insert_row(int at, char *s, size_t len){
    if (at < 0 || at > E.num_rows) return;

    E.row = realloc(E.row, sizeof(Erow) * (E.num_rows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(Erow) * (E.num_rows - at));
    for (int j = at + 1; j <= E.num_rows; j++) E.row[j].idx++;

    E.row[at].idx = at;

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editor_update_row(&E.row[at]);

    E.num_rows++;
    E.dirty++;
}

void editor_free_row(Erow *row){
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editor_del_row(int at){
    if (at < 0 || at >= E.num_rows) return;
    editor_free_row(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(Erow) * (E.num_rows - at - 1));
    for (int j = at; j < E.num_rows - 1; j++) E.row[j].idx--;
    E.num_rows--;
    E.dirty++;
}

void editor_row_insert_char(Erow *row, int at, int c){
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
    E.dirty++;
}

void editor_row_appen_string(Erow *row, char *s, size_t len){
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    editor_update_row(row);
    E.dirty++;
}

void editor_row_del_char(Erow *row, int at){
    if (at < 0 || at > row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    E.dirty++;
}

//---editor opperations---
void editor_insert_char(int c){
    if (E.cy == E.num_rows){
        editor_insert_row(E.num_rows, "", 0);
    }
    editor_row_insert_char(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editor_del_char() {
    if (E.cy == E.num_rows) return;
    if (E.cx == 0 && E.cy == 0) return;

    Erow *row = &E.row[E.cy];
    if (E.cx > 0){
        editor_row_del_char(row, E.cx - 1);
        E.cx--;
    }else{
        E.cx = E.row[E.cy - 1].size;
        editor_row_appen_string(&E.row[E.cy - 1], row->chars, row->size);
        editor_del_row(E.cy);
        E.cy--;
    }
}

void editor_insert_new_line(){
    if (E.cx == 0){
        editor_insert_row(E.cy, "", 0);
    }else{
        Erow *row = &E.row[E.cy];
        editor_insert_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }
    E.cy++;
    E.cx = 0;
}

//---file i/o---

char *editor_rows_to_string(int *buflen){
    int totlen = 0;
    int j;
    for (j = 0; j < E.num_rows; j++){
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for(j = 0; j < E.num_rows; j++){
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editor_open(char *file_name){
    free(E.filename);
    E.filename = strdup(file_name);

    editor_select_syntax_highlight();

    FILE *fp = fopen(file_name, "r");
    if (!fp) die("fopen"); // fp is NULL

    char *line = NULL;
    size_t linecap = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &linecap, fp)) != -1){
        while (line_len > 0 && (line[line_len - 1] == '\n' 
                || line[line_len - 1] == '\r'))
            line_len--;

        editor_insert_row(E.num_rows, line, line_len);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editor_save(){
    if (E.filename == NULL){
        E.filename = editor_prompt("Save as: %s", NULL);
        if (E.filename == NULL){
            editor_set_status_message("Save aborted");
            return;
        }
        editor_select_syntax_highlight();
    }

    int len;
    char *buf = editor_rows_to_string(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1){
        if (ftruncate(fd, len) != -1){
            if (write(fd, buf, len) == len){
                close(fd);
                free(buf);
                E.dirty = 0;
                editor_set_status_message("file %s saved to disk", E.filename);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editor_set_status_message("Can't save! I/O error: %s", strerror(errno));
}

//---find---
void editor_find_callback(char *query, int key){
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl){
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }


    if (key == '\r' || key == '\x1b'){
        last_match = -1;
        direction = 1;
        return;
    }else if (key == ARROW_RIGHT || key == ARROW_DOWN){
        direction = 1;
    }else if (key == ARROW_LEFT || key == ARROW_UP){
        direction = -1;
    }else{
        last_match = -1;
        direction = 1;
    }
    if (last_match == -1) direction = 1;
    int current = last_match;
    int i;
    for (i = 0; i < E.num_rows; i++){
        current += direction;
        if (current == -1) current = E.num_rows - 1;
        else if (current == E.num_rows) current = 0;

        Erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match){
            last_match = current;
            E.cy = current;
            E.cx = editor_row_rx_to_cx(row, match - row->render);
            E.row_off = E.num_rows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }

}

void editor_find(){
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_col_off = E.col_off;
    int saved_row_off = E.row_off;

    char *query = editor_prompt("Search: %s (Use ESC/Arrows/Enter)", editor_find_callback);
    if (query) {
        free(query);
    }else{
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.col_off = saved_col_off;
        E.row_off = saved_row_off;
    }
  
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
                "JEDIT -- VERSION %s", VERSION);

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
            
            char *c = &E.row[file_row].render[E.col_off];
            unsigned char *hl = &E.row[file_row].hl[E.col_off];
            int current_color = -1;
            int j;
            for (j = 0; j < len; j++){
                if (iscntrl(c[j])){
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    ab_append(ab, "\x1b[7m", 4); //invert colors
                    ab_append(ab, &sym, 1);
                    ab_append(ab, "\x1b[m", 3); // reset colors
                    // Need to set color back after reset
                    if (current_color != -1){
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        ab_append(ab, buf, clen);
                    }

                }else if (hl[j] == HL_NORMAL){
                    if (current_color != -1){
                        ab_append(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    ab_append(ab, &c[j], 1);
                }else{
                    int color = editor_syntax_to_color(hl[j]);
                    if (color != current_color){
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        ab_append(ab, buf, clen);
                    }
                    ab_append(ab, &c[j], 1);
                }

            }
            ab_append(ab, "\x1b[39m", 5); // reset to default color
        }

        ab_append(ab, "\x1b[K", 3);
        ab_append(ab, "\r\n", 2);
        
    }
}

void editor_draw_status_bar(Abuf *ab){
    ab_append(ab, "\x1b[7m", 4); // Inverted colors
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.num_rows,
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", 
        E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.num_rows);
    
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
char *editor_prompt(char *prompt, void (*callback)(char *, int)){
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1){
        editor_set_status_message(prompt, buf);
        editor_refresh_screen();

        int c = editor_read_key();
        if (c == DEL_KEY || c == BACKSPACE){
            if (buflen != 0) buf[--buflen] = '\0';
        }else if (c == '\x1b'){
            editor_set_status_message("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        }else if (c == '\r') {
            if (buflen != 0){
                editor_set_status_message("");
                if (callback) callback(buf, c);
                return buf;
            }
        }else if (!iscntrl(c) && c < 128){
            if (buflen == bufsize -1){
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c; // same as buf[buflen] = c; buflen++;
            buf[buflen] = '\0';
        }

        if (callback) callback(buf, c);
    }

}

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
    static int quit_times = QUIT_TIMES;
    int c = editor_read_key();

    switch(c){
        // Enter Key
        case '\r':
            editor_insert_new_line();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0){
                editor_set_status_message("WARNING!! File has unsaved changes. "
                    "Press Cntrl-Q %d more times to quit", quit_times);
                    quit_times--;
                    return;
            }

            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        
        case CTRL_KEY('s'):
            editor_save();
            break;

        case CTRL_KEY('f'):
            editor_find();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editor_move_cursor(ARROW_RIGHT);
            editor_del_char();
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

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editor_insert_char(c);
            break;
    }

    quit_times = QUIT_TIMES;
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
    E.dirty = 0;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;

    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1){
        die("get_window_size");
    }
    E.screen_rows -= 2;
}

int main(int argc, char *argv[]){
    system("clear");
    enable_raw_mode();

    init_editor();
    if (argc >= 2){
        editor_open(argv[1]);
    }

    editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1){
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
