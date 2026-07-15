#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <regex.h>
#include <fcntl.h>      // open()
#include <sys/stat.h>   // mkfifo()
#include <sys/ioctl.h>  // Control tamaño de pantalla

// --- Macros de Teclas ---

#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_FILENAME 256
#define FIFO_PATH "/tmp/CopyPaste_fifo"

// --- Estructuras de Datos (Memoria Dinámica) ---

typedef struct Linea {
    char *texto;            // Texto real de la línea
    int longitud;           // Longitud de linea
    int capacidad;          // Capacidad máxima del texto
    char *texto_resaltado;  // Copia con códigos ANSI para syntax highlighting
    struct Linea *siguiente;
    struct Linea *anterior;
} Linea;

// --- Estructura para reglas de sintaxis (cargadas desde archivo) ---

typedef struct {
    char ext[10];           // extensión (ej. "c", "py")
    char **keywords;        // array de palabras clave
    char **colors;          // array de códigos ANSI (sin el \x1b[)
    int count;              // número de palabras
} SyntaxRule;

// --- Estado Global del Editor ---

typedef struct EstadoEditor {
    int cx, cy;                  // Posición del cursor en pantalla
    int filas_totales;           // Cantidad de filas en el documento
    int rowoff, coloff;          // Offsets para el scrolling
    int screenrows, screencols;  // Tamaño de la terminal
    Linea *primer_linea;         // Primera Línea
    Linea *linea_actual;
    char filename[MAX_FILENAME];

    // Configuración (.editorrc)
    char bg_color[20];
    char fg_color[20];
    int mostrar_reloj;
    int autosave_time;

    // Hilos y Sincronización
    pthread_mutex_t mutex_buffer;
    int running;

    // Reglas de sintaxis (se cargan desde ~/.editor_syntax.conf)
    SyntaxRule syntax_rules[10];
    int syntax_rule_count;
} EstadoEditor;

EstadoEditor E; // Instancia global del estado

struct termios orig_termios;

// --- Declaraciones de Funciones ---

void editor_refresh_screen();
void editor_draw_footer(const char *mensaje);
void die(const char *s);
void editor_save_to_disk(void);
void load_syntax_config();

// --- Manejo de errores ---

void die(const char *s) {
    perror(s);
    exit(1);
}

// --- Manejo de la Terminal (SO a bajo nivel) ---

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[2J\x1b[H"); // Limpiar pantalla al salir
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON |
                      IGNBRK | PARMRK | INLCR | IGNCR);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) perror("tcsetattr");
}

// --- Manejo pantalla de terminal ---

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        perror("Error en ejecucion de ioctl");
        return -1;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

// --- Manejo de Memoria ---

void append_line(const char *s, size_t len) {
    pthread_mutex_lock(&E.mutex_buffer);

    Linea *nueva = malloc(sizeof(Linea));
    nueva->texto = malloc(len + 1);
    memcpy(nueva->texto, s, len);
    nueva->texto[len] = '\0';
    nueva->longitud = len;
    nueva->capacidad = len + 1;
    nueva->texto_resaltado = NULL;
    nueva->siguiente = NULL;

    if (E.primer_linea == NULL) {
        nueva->anterior = NULL;
        E.primer_linea = nueva;
        E.linea_actual = nueva;
    } else {
        Linea *temp = E.primer_linea;
        while (temp->siguiente != NULL) temp = temp->siguiente;
        temp->siguiente = nueva;
        nueva->anterior = temp;
    }

    E.filas_totales++;
    pthread_mutex_unlock(&E.mutex_buffer);
}

void free_all_lines(void) {
    Linea *actual = E.primer_linea;
    while (actual != NULL) {
        Linea *siguiente = actual->siguiente;
        free(actual->texto);
        free(actual->texto_resaltado);
        free(actual);
        actual = siguiente;
    }
    E.primer_linea = NULL;
    E.linea_actual = NULL;
}

// --- Configuración e Inicialización ---

void load_config() {
    strcpy(E.bg_color, "40");
    strcpy(E.fg_color, "37");
    E.mostrar_reloj = 1;
    E.autosave_time = 15;
    strcpy(E.filename, "sinnombre.txt");

    FILE *fp = fopen(".editorrc", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "mostrar_reloj=", 14) == 0) E.mostrar_reloj = atoi(line + 14);
            if (strncmp(line, "autoguardado=", 13) == 0) E.autosave_time = atoi(line + 13);
            if (strncmp(line, "bg_color=", 9) == 0) {
                strncpy(E.bg_color, line + 9, strcspn(line + 9, "\r\n"));
            }
        }
        fclose(fp);
    }
}

// --- Carga de configuración de sintaxis desde ~/.editor_syntax.conf ---

void load_syntax_config() {
    E.syntax_rule_count = 0;
    const char *home = getenv("HOME");
    if (!home) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/.editor_syntax.conf", home);
    FILE *fp = fopen(path, "r");

    if (!fp) {
        // Valores por defecto para C
        SyntaxRule *r = &E.syntax_rules[E.syntax_rule_count++];
        strcpy(r->ext, "c");
        r->count = 8;
        r->keywords = malloc(8 * sizeof(char*));
        r->colors   = malloc(8 * sizeof(char*));
        char *def_kw[] = {"int","char","void","return","if","else","for","while"};
        char *def_col[] = {"31","34","35","36","33","33","32","32"};
        for (int i = 0; i < 8; i++) {
            r->keywords[i] = strdup(def_kw[i]);
            r->colors[i]   = strdup(def_col[i]);
        }
        // Para Python
        r = &E.syntax_rules[E.syntax_rule_count++];
        strcpy(r->ext, "py");
        r->count = 10;
        r->keywords = malloc(10 * sizeof(char*));
        r->colors   = malloc(10 * sizeof(char*));
        char *py_kw[] = {"def","return","if","else","for","while","import","class","None","print"};
        char *py_col[] = {"33","36","33","33","32","32","34","35","31","36"};
        for (int i = 0; i < 10; i++) {
            r->keywords[i] = strdup(py_kw[i]);
            r->colors[i]   = strdup(py_col[i]);
        }
        return;
    }

    char line[256];
    SyntaxRule *current = NULL;
    while (fgets(line, sizeof(line), fp)) {
        // Eliminar saltos de línea
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '[') {
            char ext[20];
            if (sscanf(line, "[%[^]]]", ext) == 1) {
                if (E.syntax_rule_count >= 10) break;
                current = &E.syntax_rules[E.syntax_rule_count++];
                strcpy(current->ext, ext);
                current->count = 0;
                current->keywords = NULL;
                current->colors = NULL;
            }
        } else if (current && strncmp(line, "keyword ", 8) == 0) {
            char kw[64], col[10];
            if (sscanf(line + 8, "%s %s", kw, col) == 2) {
                current->count++;
                current->keywords = realloc(current->keywords, current->count * sizeof(char*));
                current->colors   = realloc(current->colors,   current->count * sizeof(char*));
                current->keywords[current->count - 1] = strdup(kw);
                current->colors[current->count - 1]   = strdup(col);
            }
        }
    }
    fclose(fp);
}

void initEditor() {
    E.cx = 0; E.cy = 0;
    E.rowoff = 0; E.coloff = 0;
    E.filas_totales = 0;
    E.primer_linea = NULL;
    E.linea_actual = NULL;
    E.running = 1;

    pthread_mutex_init(&E.mutex_buffer, NULL);

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        E.screenrows = 24;
        E.screencols = 80;
    }
    E.screenrows -= 2; // Reservar espacio para cabecera y footer

    load_config();
    load_syntax_config();   // Cargar reglas de sintaxis

    if (mkfifo(FIFO_PATH, 0644) == -1 && errno != EEXIST) {
        perror("mkfifo");
    }
}

// --- Guardado real (usado por autosave_thread y por Ctrl+S) ---

void editor_save_to_disk(void) {
    FILE *out = fopen(E.filename, "w");
    if (out) {
        Linea *tmp = E.primer_linea;
        while (tmp != NULL) {
            fwrite(tmp->texto, sizeof(char), tmp->longitud, out);
            fputc('\n', out);
            tmp = tmp->siguiente;
        }
        fclose(out);
    } else {
        perror("Error en fopen");
    }
}

// --- HILOS (Threads) ---

void *autosave_thread(void *arg) {
    (void)arg;
    while (E.running) {
        sleep(E.autosave_time);
        pthread_mutex_lock(&E.mutex_buffer);
        editor_save_to_disk();
        pthread_mutex_unlock(&E.mutex_buffer);
        editor_draw_footer(" [Auto-Guardado realizado] ");
    }
    return NULL;
}

// --- Resaltado de sintaxis optimizado (una sola regex con alternancia) ---

static void resaltar_linea(Linea *linea, SyntaxRule *rule) {
    free(linea->texto_resaltado);
    linea->texto_resaltado = NULL;

    if (rule == NULL || rule->count == 0 || linea->longitud == 0) return;

    // Construir el patrón: \b(pal1|pal2|...)\b
    char pattern[1024] = "\\b(";
    size_t pos = 3;
    for (int i = 0; i < rule->count; i++) {
        if (i > 0) {
            pattern[pos++] = '|';
            pattern[pos] = '\0';
        }
        strcpy(pattern + pos, rule->keywords[i]);
        pos += strlen(rule->keywords[i]);
    }
    strcat(pattern, ")\\b");

    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) return;

    const char *cursor = linea->texto;
    size_t usado = 0;
    size_t cap = (size_t)linea->longitud * 2 + 256;
    char *salida = malloc(cap);
    salida[0] = '\0';

    regmatch_t pmatch[1];
    while (*cursor != '\0') {
        if (regexec(&regex, cursor, 1, pmatch, 0) == 0 && pmatch[0].rm_so == 0) {
            int len = pmatch[0].rm_eo - pmatch[0].rm_so;
            // Extraer la palabra para buscar su color
            char word[64];
            snprintf(word, sizeof(word), "%.*s", len, cursor);
            char *color = NULL;
            for (int i = 0; i < rule->count; i++) {
                if (strcmp(word, rule->keywords[i]) == 0) {
                    color = rule->colors[i];
                    break;
                }
            }
            if (color) {
                usado += snprintf(salida + usado, cap - usado, "\x1b[%sm", color);
                usado += snprintf(salida + usado, cap - usado, "%.*s", len, cursor);
                usado += snprintf(salida + usado, cap - usado, "\x1b[0m");
            } else {
                usado += snprintf(salida + usado, cap - usado, "%.*s", len, cursor);
            }
            cursor += len;
        } else {
            // Copiar un carácter normal
            if (usado + 2 > cap) {
                cap *= 2;
                salida = realloc(salida, cap);
            }
            salida[usado++] = *cursor;
            salida[usado] = '\0';
            cursor++;
        }
    }
    regfree(&regex);
    linea->texto_resaltado = salida;
}

void *syntax_thread(void *arg) {
    (void)arg;
    while (E.running) {
        sleep(2);
        pthread_mutex_lock(&E.mutex_buffer);

        // Buscar regla para la extensión actual
        SyntaxRule *rule = NULL;
        const char *ext = strrchr(E.filename, '.');
        if (ext) {
            ext++; // saltar el punto
            for (int i = 0; i < E.syntax_rule_count; i++) {
                if (strcmp(E.syntax_rules[i].ext, ext) == 0) {
                    rule = &E.syntax_rules[i];
                    break;
                }
            }
        }
        if (rule) {
            Linea *l = E.primer_linea;
            while (l != NULL) {
                resaltar_linea(l, rule);
                l = l->siguiente;
            }
        }
        pthread_mutex_unlock(&E.mutex_buffer);
    }
    return NULL;
}

// --- Interfaz Gráfica (ANSI) ---

void editor_draw_header(char *buffer) {
    char header[256];
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);

    char reloj[16] = "";
    if (E.mostrar_reloj) sprintf(reloj, " | %02d:%02d", tm.tm_hour, tm.tm_min);

    snprintf(header, sizeof(header),
             "\x1b[7m %s - %d lineas | Col: %d, Fila: %d%s \x1b[K\x1b[m\r\n",
             E.filename, E.filas_totales, E.cx, E.cy, reloj);

    strcat(buffer, header);
}

void editor_draw_footer(const char *msg) {
    printf("\x1b[%d;1H", E.screenrows + 2);
    printf("\x1b[7m");
    if (msg != NULL) {
        printf("%-80s", msg);
    } else {
        printf(" ^O Abrir | ^S Guardar | ^A G. Como | ^C Copiar | ^V Pegar | ^X Cortar | ^Q Salir ");
    }
    printf("\x1b[m\x1b[K");
}

void editor_refresh_screen() {
    char *buffer = malloc(8192);
    strcpy(buffer, "\x1b[?25l");
    strcat(buffer, "\x1b[H");

    editor_draw_header(buffer);

    pthread_mutex_lock(&E.mutex_buffer);

    Linea *l = E.primer_linea;
    for (int y = 0; y < E.screenrows; y++) {
        if (l != NULL) {
            const char *a_mostrar = (l->texto_resaltado != NULL) ? l->texto_resaltado : l->texto;
            strncat(buffer, a_mostrar, E.screencols > 0 ? (size_t)E.screencols * 8 : 80);
            l = l->siguiente;
        } else {
            strcat(buffer, "~");
        }
        strcat(buffer, "\x1b[K\r\n");
    }

    pthread_mutex_unlock(&E.mutex_buffer);

    printf("%s", buffer);
    editor_draw_footer(NULL);

    printf("\x1b[%d;%dH", (E.cy - E.rowoff) + 2, (E.cx - E.coloff) + 1);
    printf("\x1b[?25h");
    fflush(stdout);

    free(buffer);
}

// --- Copiar / Pegar vía FIFO ---

void accion_copiar(void) {
    if (E.linea_actual == NULL) return;
    int fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        editor_draw_footer(" No hay proceso leyendo el portapapeles ");
        return;
    }
    if (E.linea_actual->texto != NULL && E.linea_actual->longitud > 0) {
        write(fd, E.linea_actual->texto, E.linea_actual->longitud);
    }
    close(fd);
    editor_draw_footer("Contenido copiado ....");
}

void accion_pegar(void) {
    if (E.linea_actual == NULL) return;
    int fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (fd == -1) return;

    char info_buffer[4096];
    ssize_t bytes_leidos = read(fd, info_buffer, sizeof(info_buffer) - 1);
    close(fd);

    if (bytes_leidos <= 0) return;
    info_buffer[bytes_leidos] = '\0';

    Linea *actual = E.linea_actual;
    int nueva_longitud = actual->longitud + (int)bytes_leidos;

    if (nueva_longitud >= actual->capacidad) {
        int nueva_capacidad = nueva_longitud + 128;
        char *nuevo_texto = realloc(actual->texto, nueva_capacidad);
        if (nuevo_texto == NULL) return;
        actual->texto = nuevo_texto;
        actual->capacidad = nueva_capacidad;
    }

    if (E.cx < actual->longitud) {
        memmove(&actual->texto[E.cx + bytes_leidos],
                &actual->texto[E.cx],
                actual->longitud - E.cx);
    }

    memcpy(&actual->texto[E.cx], info_buffer, bytes_leidos);
    actual->longitud = nueva_longitud;
    actual->texto[nueva_longitud] = '\0';
    E.cx += (int)bytes_leidos;

    editor_draw_footer("Contenido pegado ....");
}

// --- Inserción básica de caracteres ---

void insertar_caracter(char c) {
    if (E.linea_actual == NULL) return;

    Linea *actual = E.linea_actual;
    int nueva_longitud = actual->longitud + 1;

    if (nueva_longitud >= actual->capacidad) {
        int nueva_capacidad = nueva_longitud + 128;
        char *nuevo_texto = realloc(actual->texto, nueva_capacidad);
        if (nuevo_texto == NULL) return;
        actual->texto = nuevo_texto;
        actual->capacidad = nueva_capacidad;
    }

    if (E.cx < actual->longitud) {
        memmove(&actual->texto[E.cx + 1], &actual->texto[E.cx], actual->longitud - E.cx);
    }

    actual->texto[E.cx] = c;
    actual->longitud = nueva_longitud;
    actual->texto[nueva_longitud] = '\0';
    E.cx++;
}

// --- Funciones de borrado ---

void borrar_caracter_izquierda() {
    if (E.linea_actual == NULL) return;

    // Si estamos al inicio de la línea, unir con la anterior
    if (E.cx == 0) {
        if (E.linea_actual->anterior == NULL) return; // No hay línea anterior

        Linea *prev = E.linea_actual->anterior;
        int prev_len = prev->longitud;
        int curr_len = E.linea_actual->longitud;

        // Reasignar memoria para concatenar
        int new_len = prev_len + curr_len + 1;
        char *new_text = realloc(prev->texto, new_len);
        if (new_text == NULL) return;

        // Copiar el contenido de la línea actual después de la anterior
        memcpy(new_text + prev_len, E.linea_actual->texto, curr_len + 1);
        prev->texto = new_text;
        prev->longitud = new_len - 1;
        prev->capacidad = new_len;

        // Ajustar punteros
        prev->siguiente = E.linea_actual->siguiente;
        if (E.linea_actual->siguiente)
            E.linea_actual->siguiente->anterior = prev;

        // Liberar la línea actual
        free(E.linea_actual->texto);
        free(E.linea_actual->texto_resaltado);
        free(E.linea_actual);

        E.linea_actual = prev;
        E.filas_totales--;
        E.cx = prev_len; // cursor al final de la línea fusionada
        return;
    }

    // Borrar carácter a la izquierda del cursor
    Linea *actual = E.linea_actual;
    if (E.cx > 0) {
        memmove(&actual->texto[E.cx - 1],
                &actual->texto[E.cx],
                actual->longitud - E.cx + 1);
        actual->longitud--;
        E.cx--;
    }
}

void borrar_caracter_derecha() {
    if (E.linea_actual == NULL) return;
    Linea *actual = E.linea_actual;
    if (E.cx < actual->longitud) {
        memmove(&actual->texto[E.cx],
                &actual->texto[E.cx + 1],
                actual->longitud - E.cx);
        actual->longitud--;
    }
}

// --- Salida limpia del programa ---

void editor_shutdown(void) {
    E.running = 0;

    pthread_mutex_lock(&E.mutex_buffer);
    free_all_lines();
    pthread_mutex_unlock(&E.mutex_buffer);

    pthread_mutex_destroy(&E.mutex_buffer);
    unlink(FIFO_PATH);

    printf("\x1b[2J\x1b[H");
}

// --- Procesamiento de Entrada (con soporte para teclas especiales) ---

void editor_process_keypress() {
    char c;
    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) return;

    // Manejo de secuencias de escape (teclas especiales)
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return;

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': // Flecha arriba
                    E.cy = (E.cy > 0) ? E.cy - 1 : 0;
                    break;
                case 'B': // Flecha abajo
                    E.cy++;
                    break;
                case 'C': // Flecha derecha
                    E.cx++;
                    break;
                case 'D': // Flecha izquierda
                    E.cx = (E.cx > 0) ? E.cx - 1 : 0;
                    break;
                case 'H': // Home
                    E.cx = 0;
                    break;
                case 'F': // End
                    if (E.linea_actual)
                        E.cx = E.linea_actual->longitud;
                    break;
                case '3': // Delete (puede ser \x1b[3~)
                    // Leer el siguiente carácter para confirmar '~'
                    char next;
                    if (read(STDIN_FILENO, &next, 1) == 1 && next == '~') {
                        borrar_caracter_derecha();
                    }
                    break;
                default:
                    break;
            }
        }
        return;
    }

    // Teclas de control y caracteres imprimibles
    switch (c) {
        case CTRL_KEY('q'): // Salir
            editor_shutdown();
            exit(0);
            break;

        case CTRL_KEY('s'): // Guardar
            editor_draw_footer(" Guardando archivo... ");
            fflush(stdout);
            pthread_mutex_lock(&E.mutex_buffer);
            editor_save_to_disk();
            pthread_mutex_unlock(&E.mutex_buffer);
            break;

        case CTRL_KEY('c'): // Copiar
            accion_copiar();
            break;

        case CTRL_KEY('v'): // Pegar
            accion_pegar();
            break;

        case 127:        // Backspace (ASCII DEL)
        case CTRL_KEY('h'): // Ctrl+H también es backspace
            borrar_caracter_izquierda();
            break;

        default:
            if (isprint((unsigned char)c)) {
                insertar_caracter(c);
            }
            break;
    }
}

// --- MAIN ---

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();

    if (argc >= 2) {
        strncpy(E.filename, argv[1], MAX_FILENAME - 1);
        E.filename[MAX_FILENAME - 1] = '\0';

        append_line("/* Bienvenido al editor (Fase 2) */", 35);
        append_line("int main() {", 12);
        append_line("    return 0;", 13);
        append_line("}", 1);
    } else {
        append_line("", 0);
    }

    pthread_t t_save, t_syntax;
    pthread_create(&t_save, NULL, autosave_thread, NULL);
    pthread_create(&t_syntax, NULL, syntax_thread, NULL);

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}