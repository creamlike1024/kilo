/*** includes ***/
// 控制系统头文件特性
#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define EDITOR_TAB_STOP 8
#define EDITOR_QUIT_TIMES 2

#define CTRL_KEY(k) ((k) & 0x1f) // 将字符上三位设置为 0 以表示 Ctrl 键

enum edidtorKey
{
    BACKSPACE = 127,
    ARROR_LEFT = 1000,
    ARROR_RIGHT,
    ARROR_UP,
    ARROR_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum editorHighlight
{
    HL_NORMAL = 0,
    HL_COMMENT,   // 单行注释
    HL_MLCOMMENT, // 多行注释
    HL_KEYWORD1,  // 关键字一
    HL_KEYWORD2,  // 关键字二
    HL_STRING,    // 字符串
    HL_NUMBER,    // 数字
    HL_MATCH      // 搜索匹配
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

/*** data ***/

struct editorSyntax
{
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags; // flag 包含该文件类型要突出显示哪些内容的标志
};

// Editor Row
typedef struct erow
{
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hl_open_comment;
} erow;

struct editorConfig
{
    int cx, cy; // 光标位置
    int rx;     // 实际渲染的光标 x 位置
    int rowoff; // 行偏移量
    int coloff; // 列偏移量
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
    struct termios orig_termios; // 终端初始属性
};

struct editorConfig E;

/*** filetypes ***/

char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL}; // 数组必须以 NULL 结尾
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL};

char *GO_HL_extensions[] = {".go", ".mod", NULL};
char *GO_HL_keywords[] = {
    "break", "default", "func", "interface", "select", "case", "defer",
    "go", "map", "struct", "chan", "else", "goto", "package", "switch",
    "const", "fallthrough", "if", "range", "type", "continue", "for",
    "import", "return", "var",
    "int|", "int8|", "int16|", "int32|", "int64|", "uint|", "uint8|", "uint16|",
    "uint32|", "uint64|", "rune|", "byte|", "uintptr|", "float32|", "float64|",
    "complex64|", "complex128|", "bool|", "string|", NULL};

char *PYTHON_HL_extensions[] = {".py", NULL};
char *PYTHON_HL_keywords[] = {
    "False", "await", "else", "import", "pass", "None", "break", "except", "in",
    "raise", "True", "class", "finally", "is", "return", "and", "continue", "for",
    "lambda", "try", "as", "def", "from", "nonlocal", "while", "assert", "del",
    "global", "not", "with", "async", "elif", "if", "or", "yield",
    "int|", "float|", "complex|", "str|", "list|", "tuple|", "range|", "dict|",
    "bool|", "set|", "frozenset|", NULL};

char *SHELL_HL_extensions[] = {".sh", NULL};
char *SHELL_HL_keywords[] = {
    "echo", "read", "set", "unset", "readonly", "shift", "export", "if", "fi",
    "else", "while", "do", "done", "for", "until", "case", "esac", "break",
    "continue", "exit", "return", "trap", "wait", "eval", "exec", "ulimit",
    "umask", NULL};

struct editorSyntax HLDB[] = {
    {"C/C++",
     C_HL_extensions,
     C_HL_keywords,
     "//", "/*", "*/",
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
    {"Golang",
     GO_HL_extensions,
     GO_HL_keywords,
     "//", "/*", "*/",
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
    {"Python",
     PYTHON_HL_extensions,
     PYTHON_HL_keywords,
     "#", NULL, NULL,
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
    {"Shell",
     SHELL_HL_extensions,
     SHELL_HL_keywords,
     "#", NULL, NULL,
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS}};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0])) // HLDB 数组长度

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

// 错误时打印错误信息并退出
void die(const char *s)
{
    // 退出时清理屏幕并重新定位光标
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

// 退出时调用的函数，恢复终端属性
void disableRawMode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

// 设置终端属性启用实模式
void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode); // atexit 来自 stdlib.h

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // 关闭输入回车转换换行、软件流控制
    // BRKINT 打开时，中断条件向程序发送 SIGINT 信号
    // INPCK 启用奇偶校验
    // ISTRIP 是每个输入字节第 8 位剥离（设为 0）
    raw.c_oflag &= ~(OPOST);                         // 关闭输出处理，如换行转换
    raw.c_cflag |= (CS8);                            // 字符大小设置为每字节 8 位
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); // 关闭 echo（回显输入字符）、规范模式、Ctrl-V 和信号
    raw.c_cc[VMIN] = 0;                              // 让 read() 返回的最小输入字节数，设为 0 让 read() 有输入时立即返回
    raw.c_cc[VTIME] = 1;                             // 返回前 read() 最长等待时间 1（单位十分之一秒）

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editorReadKey()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        if (nread == -1 && errno != EAGAIN)
            die("read"); // Cygwin 中 read() 超时还会返回 errno 为 EAGAIN
    }

    // 对方向键映射为光标控制键
    if (c == '\x1b')
    {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return ARROR_UP;
                case 'B':
                    return ARROR_DOWN;
                case 'C':
                    return ARROR_RIGHT;
                case 'D':
                    return ARROR_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        }
        else if (seq[0] == '0')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }
        return '\x1b';
    }
    else
    {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    // N 命令获取光标当前位置
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break; // 读取到 R 字符时跳出
        i++;
    }

    buf[i] = '\0'; // 确保缓冲区是一个有效的字符串

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1; // 检查相应是否以转义字符开始
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1; // 匹配终端响应字符串，结果放入 rows 和 cols

    return 0;
}

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws; // winsize 来自 <sys/ioctl.h>

    // ioctl 成功获取到终端行列数时放在 ws 结构体里，TIOCGWINSZ 命令获取窗口大小
    // ioctl 失败是返回 -1，还要确保列数不能为 0
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        // 如果 ioctl 失败，使用另一种方案获取屏幕大小
        // C 命令让光标右移，B 命令让光标左移，999 确保到底屏幕右下角、
        // 不使用 H 命令时因为 H 命令在标准中未定义光标移出屏幕时会发生什么，而 C B 命令有标准定义不会越出屏幕
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, cols);
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlighting ***/

int is_separator(int c)
{
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row)
{
    // hl 和 rsize 一样大
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    // 没有文件类型，不更新高亮
    if (E.syntax == NULL)
        return;

    char **keywords = E.syntax->keywords;

    // 是否有单行注释开始符
    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1;                                                       // 前一个字符是否为分隔符
    int in_string = 0;                                                      // 当前是否在字符串中
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment); // 是否在多行注释中

    int i = 0;
    while (i < row->rsize)
    {
        char c = row->render[i];
        // prev_hl 设置为前一个字符串的突出显示类型
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        // 文件类型存在单行注释高亮且此时不在字符串和多行注释中
        if (scs_len && !in_string && !in_comment)
        {
            if (!strncmp(&row->render[i], scs, scs_len))
            {
                // 将整行其余部分高亮
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        // 文件类型存在多行注释且当前不在字符串中
        if (mcs_len && mce_len && !in_string)
        {
            if (in_comment)
            {
                row->hl[i] = HL_MLCOMMENT;
                // 是否处于多行注释末尾
                if (!strncmp(&row->render[i], mce, mce_len))
                {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                }
                else
                {
                    i++;
                    continue;
                }
            }
            // 是否处于多行注释开头
            else if (!strncmp(&row->render[i], mcs, mcs_len))
            {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        // 检查是否突出显示当前文件类型的字符串
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS)
        {
            if (in_string)
            {
                row->hl[i] = HL_STRING;
                // 突出显示反斜杠后面的字符
                if (c == '\\' && i + 1 < row->rsize)
                {
                    row->hl[i] = HL_STRING;
                    i += 2;
                    continue;
                }
                // 在字符串中时，遇到 " 或 ' 说明结束字符串
                if (c == in_string)
                    in_string = 0;
                i++;
                prev_sep = 1; // 结束引号视为分隔符
                continue;
            }
            else
            {
                if (c == '"' || c == '\'')
                {
                    in_string = c; // 设置为字符串开始/结束字符
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        // 检查是否突出显示当前文件类型的数字
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS)
        {
            // 要突出显示数字，前一个字符为分隔符或者也是突出显示的数字，包含小数点的数字也考虑
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER))
            {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        // 关键词需要有分隔符
        if (prev_sep)
        {
            int j;
            for (j = 0; keywords[j]; j++)
            {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2)
                    klen--;

                // 关键词之后也要有分隔符
                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen]))
                {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL)
            {
                prev_sep = 0;
                continue;
            }
        }

        // 如果没有突出显示当前字符
        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment; // 该行是否以未闭合的多行注释结束
    if (changed && row->idx + 1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx + 1]);
}

int editorSyntaxToColor(int hl)
{
    switch (hl)
    {
    case HL_COMMENT:
    case HL_MLCOMMENT:
        return 36; // 青色
    case HL_KEYWORD1:
        return 33; // 黄色
    case HL_KEYWORD2:
        return 32; // 绿色
    case HL_STRING:
        return 35; // 洋红色
    case HL_NUMBER:
        return 31; // 红色
    case HL_MATCH:
        return 34; // 蓝色
    default:
        return 37;
    }
}

void editorSelectSyntaxHighlight()
{
    E.syntax = NULL;
    if (E.filename == NULL)
        return;

    char *ext = strrchr(E.filename, '.'); // 返回指向字符串中最后一个字符出现的指针

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++)
    {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i])
        {
            int is_ext = (s->filematch[i][0] == '.'); // 是否有拓展名
            // 检查文件类型是否与高亮数据库中的匹配，有拓展名比较拓展名，无拓展名比较文件名
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) || (!is_ext && strstr(E.filename, s->filematch[i])))
            {
                E.syntax = s;

                // 确保文件类型更改时 (open, save) 突出显示立即更改
                int filerow;
                for (filerow = 0; filerow < E.numrows; filerow++)
                {
                    editorUpdateSyntax(&E.row[filerow]);
                }
                return;
            }
            i++;
        }
    }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx)
{
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++)
    {
        if (row->chars[j] == '\t')
            // 到达下一个制表位
            rx += (EDITOR_TAB_STOP - 1) - (rx % EDITOR_TAB_STOP);
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx)
{
    int cur_rx = 0;
    int cx;
    // 边遍历边计算 rx，当计算出的 rx 和 给定 rx 相同时，返回此时 cx
    for (cx = 0; cx < row->size; cx++)
    {
        if (row->chars[cx] == '\t')
            cur_rx += (EDITOR_TAB_STOP - 1) - (cur_rx % EDITOR_TAB_STOP);
        cur_rx++;

        if (cur_rx > rx)
            return cx;
    }
    return cx;
}

void editorUpdateRow(erow *row)
{
    // 由于 tab 转换为 8 个空格，申请的内存空间也要增大
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t')
            tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs * (EDITOR_TAB_STOP - 1) + 1);

    // 复制字符串
    int idx = 0;
    for (j = 0; j < row->size; j++)
    {
        // 将 tab 转换为 8 个空格
        if (row->chars[j] == '\t')
        {
            // 每个制表符必须让光标向前移动至少一列
            row->render[idx++] = ' ';
            // 到达制表位
            while (idx % EDITOR_TAB_STOP != 0)
                row->render[idx++] = ' ';
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len)
{
    if (at < 0 || at > E.numrows)
        return;

    // 重新分配增加一行之后的内存
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    // 后面所有行向后移动，腾出新行位置
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++)
        E.row[j].idx++;

    E.row[at].idx = at;

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    // 将给定字符串复制到新行
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);

    E.numrows++; // 表示行数 +1
    E.dirty++;   // 脏位
}

void editorFreeRow(erow *row)
{
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at)
{
    // 检查位置合法性
    if (at < 0 || at >= E.numrows)
        return;
    editorFreeRow(&E.row[at]);
    // 将后面所有行向前移动
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++)
        E.row[j].idx--;
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c)
{
    // 检查字符位置是否合规
    if (at < 0 || at > row->size)
        at = row->size;
    row->chars = realloc(row->chars, row->size + 2); // 字符 + null
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);

    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len)
{
    // +1 是包括空字节
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at)
{
    // 检查字符位置是否合规
    if (at < 0 || at >= row->size)
        return;
    // 删除字符：移动后面的所有字符
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c)
{
    // 如果光标位于文件末尾的波浪线上，需要插入字符前添加一个新行
    if (E.cy == E.numrows)
    {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline()
{
    // 如果在第一行开头，在所在行之前插入空白行
    if (E.cx == 0)
    {
        editorInsertRow(E.cy, "", 0);
    }
    else
    {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    // 移动光标到行开头
    E.cy++;
    E.cx = 0;
}

void editorDelChar()
{
    // 如果光标超过文件内容，无需删除操作
    if (E.cy == E.numrows)
        return;
    // 光标位于文件开头，不响应删除
    if (E.cx == 0 && E.cy == 0)
        return;

    erow *row = &E.row[E.cy];

    if (E.cx > 0)
    {
        // Backspace 删除光标左侧字符
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    else
    {
        // 光标定位到上一行末尾
        E.cx = E.row[E.cy - 1].size;
        // 当前行拼接到上一行结尾
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        // 删除当前行
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen)
{
    // 写入到磁盘中的字符串需要每行加上一个换行符，计算所需内存
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen;

    // 分配内存
    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < E.numrows; j++)
    {
        // 复制字符串并在行尾加上换行符
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename)
{
    free(E.filename);
    // strdup 自动分配内存并复制字符串
    E.filename = strdup(filename);

    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    // 循环读入行，getline 到达文件末尾时返回 -1
    // getline 自动为 line 分配内存，分配大小写入 linecap，是行的长度
    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        // 去掉换行回车，因为编辑器每行都表示一行文本
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
                               line[linelen - 1] == '\r'))
            linelen--;

        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);

    // 由于打开文件时会调用 editorInsertRow() 修改了脏位，需要重置
    E.dirty = 0;
}

void editorSave()
{
    if (E.filename == NULL)
    {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL)
        {
            editorSetStatusMessage("Save aborted");
            return;
        }
        editorSelectSyntaxHighlight();
    }

    int len;
    char *buf = editorRowsToString(&len);

    // 以读写模式打开，不存的则创建
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    // 错误检查
    if (fd != -1)
    {
        // 将文件大小设为 len
        if (ftruncate(fd, len) != -1)
        {
            if (write(fd, buf, len) == len)
            {
                close(fd);
                free(buf);
                E.dirty = 0; // 保存后重置脏位
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char *query, int key)
{
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl)
    {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b')
    {
        last_match = -1;
        direction = 1;
        return;
    }
    else if (key == ARROR_RIGHT || key == ARROR_DOWN)
    {
        direction = 1;
    }
    else if (key == ARROR_LEFT || key == ARROR_UP)
    {
        direction = -1;
    }
    else
    {
        last_match = -1;
        direction = 1;
    }

    // 没有最后一个匹配项时从头部开始向前搜索
    if (last_match == -1)
        direction = 1;
    int current = last_match;
    int i;
    // 逐行查找字符串
    for (i = 0; i < E.numrows; i++)
    {
        current += direction;
        // 向后搜索时从文件开头回到末尾
        if (current == -1)
            current = E.numrows - 1;
        // 向后搜索时从文件末尾回到开头
        else if (current == E.numrows)
            current = 0;

        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match)
        {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render); // 获取偏移量
            E.rowoff = E.numrows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind()
{
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
    if (query)
    {
        free(query);
    }
    else
    {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/*** append buffer ***/

struct abuf
{
    char *b; // 指向内存缓冲区的指针
    int len; // 长度
};

// 代表一个空缓冲区
#define ABUF_INIT \
    {             \
        NULL, 0   \
    }

// 写入缓冲区
void abAppend(struct abuf *ab, const char *s, int len)
{
    // 申请新的内存块，大小是当前缓冲区增加 len 的长度
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
        return;
    // 复制字符串到新缓冲区
    memcpy(&new[ab->len], s, len);
    // 更新指针和长度
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** output ***/

void editorScroll()
{
    E.rx = E.cx;
    // 计算有可能存在的制表符对应光标位置
    if (E.cy < E.numrows)
    {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    // 光标在顶部向上移动
    if (E.cy < E.rowoff)
    {
        E.rowoff = E.cy;
    }
    // 光标是否在窗口底部向下滚动，rowoff 是屏幕顶部的行在文件中的行号
    if (E.cy >= E.rowoff + E.screenrows)
    {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    // 光标在左边缘向左移动
    if (E.rx < E.coloff)
    {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols)
    {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screenrows; y++)
    {
        // 当前光标所在行
        int filerow = y + E.rowoff;
        // 当前光标所在行是否在文本缓冲区内
        if (filerow >= E.numrows)
        {
            // 仅当文本缓冲区为空时，才显示欢迎信息
            if (E.numrows == 0 && y == E.screenrows / 3)
            {
                char welcome[80];
                // 将格式化字符串写入缓冲区，并返回长度
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          "Kilo editor -- version %s", KILO_VERSION);
                // 如果字符串长度大于终端列数，截断
                if (welcomelen > E.screencols)
                    welcomelen = E.screencols;
                // 计算让字符串居中的填充长度
                int padding = (E.screencols - welcomelen) / 2;
                // 第一个填充字符串打印波浪号，剩下打印空格
                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
            else
            {
                // 非文件内容的行，画波浪线
                abAppend(ab, "~", 1);
            }
        }
        else
        {
            // 减去列偏移量的行长度
            int len = E.row[filerow].rsize - E.coloff;
            // 如果当前行已经到达末端（len <= 0），若 len < 0，令 len = 0 传递给 abAppend
            if (len < 0)
                len = 0;
            // 大于终端列数则截断
            if (len > E.screencols)
                len = E.screencols;
            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;
            int j;
            for (j = 0; j < len; j++)
            {
                if (iscntrl(c[j]))
                {
                    // 替代不可见字符并颜色反转打印
                    char sym = (c[j] <= 26 ? '@' + c[j] : '?');
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    if (current_color != -1)
                    {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                }
                else if (hl[j] == HL_NORMAL)
                {
                    if (current_color != -1)
                    {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                }
                else
                {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color)
                    {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5);
        }

        // K 指令擦除当前行的一部分
        // 2 擦除整行，1 和 0 擦除光标右侧的行部分，默认参数 0
        abAppend(ab, "\x1b[K", 3);

        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab)
{
    // m 命令设置文本属性，参数：
    // 1 粗体，4 下划线，5 闪烁，7 反转颜色，0 清除所有属性（默认参数）
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    // 文件名以及是否修改提示
    int len = snprintf(status, sizeof(status), "%.20s %s",
                       E.filename ? E.filename : "[No name]",
                       E.dirty ? "(modified)" : "");
    // 行号和文件类型
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                        E.syntax ? E.syntax->filetype : "No filetype", E.cy + 1, E.numrows);

    if (len > E.screencols)
        len = E.screencols;
    abAppend(ab, status, len);

    while (len < E.screencols)
    {
        if (E.screencols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        }
        else
        {
            // 用空格填充，直到打印第二个状态字符串
            abAppend(ab, " ", 1);
            len += 1;
        }
    }
    // 关闭颜色反转
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols)
        msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen()
{
    editorScroll();

    struct abuf ab = ABUF_INIT;

    // 刷新屏幕内容之前隐藏光标，该控制指令非 VT100 标准
    abAppend(&ab, "\x1b[?25l", 6);

    // // 写 4 字节到终端，第一个字节是 \x1b 即转义字符 27，其它三个字节为[2J]
    // // \x1b[ 表示执行终端文本格式设置
    // // J 命令清除屏幕，参数 2 表示清除整个屏幕
    // // 使用 VT100 转义序列，为了适应更多终端可使用 ncurses 库计算转义序列
    // abAppend(&ab, "\x1b[2J", 4);

    // H 命令定位光标，H 命令有两个参数分别表示定位行号和列号
    // 多个参数用 ; 分隔，如 [12;40H，让 80x24 大小的终端光标定位在第一行和第一列
    // H 默认参数都为 1，这里省略参数
    abAppend(&ab, "\x1b[H", 3);

    // 重新显示光标
    abAppend(&ab, "\x1b[?25h", 6);

    editorDrawRows(&ab);
    // 绘制状态栏
    editorDrawStatusBar(&ab);
    // 绘制消息栏
    editorDrawMessageBar(&ab);

    // 画完之后，重新移动光标位置
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    // 缓冲区内容写到终端
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;                                           // 可变参数列表
    va_start(ap, fmt);                                    // 初始化可变参数列表，fmt 为最后一个非可变参数
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap); // 使用可变参数格式化字符串
    va_end(ap);                                           // 释放 ap
    E.statusmsg_time = time(NULL);                        // 获取当前时间存储在 statusmsg_time 中
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        // 响应输入文件名时的删除操作
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE)
        {
            if (buflen != 0)
                buf[--buflen] = '\0';
        }
        else if (c == '\x1b') // 按下 esc 键取消输入文件名
        {
            editorSetStatusMessage("");
            if (callback)
                callback(buf, c);
            free(buf);
            return NULL;
        }
        else if (c == '\r') // 按下回车时且输入不为空，清空消息信息并返回
        {
            if (buflen != 0)
            {
                editorSetStatusMessage("");
                if (callback)
                    callback(buf, c);
                return buf;
            }
        }
        // 输入可见字符时，添加到 buf
        else if (!iscntrl(c) && c < 128)
        {
            // 如果 buf 快满了，以原来的大小 x2 重新分配 buf
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            // 确保 buf 结尾为 \0
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback)
            callback(buf, c);
    }
}

void editorMoveCursor(int key)
{
    // 确保光标 cy 在文件实际内容行上而不是超过了最后一行
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key)
    {
    case ARROR_LEFT:
        if (E.cx != 0)
        {
            E.cx--;
        }
        else if (E.cy > 0)
        {
            // 不在第一行时，移动光标到上一行结尾
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROR_RIGHT:
        if (row && E.cx < row->size)
        {
            E.cx++;
        }
        else if (row && E.cx == row->size)
        {
            // 不在最后一行时，移动光标到下一行开头
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROR_UP:
        if (E.cy != 0)
        {
            E.cy--;
        }
        break;
    case ARROR_DOWN:
        if (E.cy < E.numrows)
        {
            E.cy++;
        }
        break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen)
    {
        E.cx = rowlen;
    }
}

void editorProcessKeypress()
{
    // 需要在 editorReadKey() 调用后仍然保持计数
    static int quit_times = EDITOR_QUIT_TIMES;

    int c = editorReadKey();

    switch (c)
    {
    case '\r':
        editorInsertNewline();
        break;
    case CTRL_KEY('x'):
        if (E.dirty && quit_times > 0)
        {
            editorSetStatusMessage("WARNING!! File has unsaved changes."
                                   "Press Ctrl-X %d more times to quit.",
                                   quit_times);
            quit_times--;
            return;
        }
        // Ctrl-q 退出时清理屏幕和定位光标
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);

        exit(0);
        break;

    case CTRL_KEY('s'):
        editorSave();
        break;

    case HOME_KEY:
        E.cx = 0;
        break;

    case END_KEY:
        // 滚动到行末尾，首先要确认在文件内容段中才响应 End 键
        if (E.cy < E.numrows)
            E.cx = E.row[E.cy].size;
        break;

    case CTRL_KEY('f'):
        editorFind();
        break;

    case BACKSPACE:
    // Ctrl-H 发送 8 （Backspace 的 ASCII 码）
    case CTRL_KEY('h'):
    case DEL_KEY:
        if (c == DEL_KEY)
            editorMoveCursor(ARROR_RIGHT);
        editorDelChar();
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    {
        if (c == PAGE_UP)
        {
            // 按页滚动，向上滚动时从屏幕顶部开始计算
            E.cy = E.rowoff;
        }
        else if (c == PAGE_DOWN)
        {
            // 定位到屏幕底部
            E.cy = E.rowoff + E.screenrows - 1;
            // 防止滚出文件内容
            if (E.cy > E.numrows)
                E.cy = E.numrows;
        }

        int times = E.screenrows;
        // 模拟按方向键次数
        while (times--)
        {
            editorMoveCursor(c == PAGE_UP ? ARROR_UP : ARROR_DOWN);
        }
        break;
    }

    // 移动光标控制键处理
    case ARROR_UP:
    case ARROR_DOWN:
    case ARROR_LEFT:
    case ARROR_RIGHT:
        editorMoveCursor(c);
        break;

    // Ctrl-L 用于终端上刷新屏幕，编辑器不需要该功能
    case CTRL_KEY('l'):
    case '\x1b':
        break;

    default:
        editorInsertChar(c);
        break;
    }

    quit_times = EDITOR_QUIT_TIMES;
}

/*** init ***/

void initEditor()
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    // 预留底部状态栏空间
    E.screenrows -= 2;
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
    {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-X = quit | Ctrl-F = find");

    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}