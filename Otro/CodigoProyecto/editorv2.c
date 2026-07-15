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
    int longitud;            // Longitud de linea
    int capacidad;            // Capacidad máxima del texto
    char *texto_resaltado;    // Copia con códigos ANSI para syntax highlighting (puede ser NULL)
    struct Linea *siguiente;  // Siguiente Línea
    struct Linea *anterior;   // Línea Previa

} Linea;


// --- Estado Global del Editor ---
// (Se separa el "tag" del struct del nombre de la variable global: mezclar
//  ambos bajo el mismo identificador "E" —como en la versión anterior—
//  es un error de sintaxis en C, no una simple cuestión de estilo.)

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

} EstadoEditor;

EstadoEditor E; // Instancia global del estado

typedef struct {
    char extension[10];
    char **keywords;
    char **colors;   // códigos ANSI (ej. "31" para rojo)
    int count;
} SyntaxRule;



struct termios orig_termios;

// --- Declaraciones de Funciones ---

void editor_refresh_screen();
void editor_draw_footer(const char *mensaje);
void die(const char *s);
void editor_save_to_disk(void); // lógica real de guardado (sin bucle infinito)
SyntaxRule syntax_rules[10];
int syntax_rule_count = 0;



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

    // Guardar atributos de terminal
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = orig_termios;

    // Desactivar flags:
    // BRKINT, ICRNL, INPCK, ISTRIP, IXON (Ctrl+S, Ctrl+Q gestionados manualmente)
    // OPOST (Procesamiento de salida)
    // ECHO, ICANON (Modo raw), IEXTEN, ISIG (Ctrl+C, Ctrl+Z gestionados manualmente)

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON |
                      IGNBRK | PARMRK | INLCR | IGNCR);

    raw.c_oflag &= ~(OPOST);

    raw.c_cflag &= ~(CSIZE | PARENB);  // <- corregido: era "c_cfloag"
    raw.c_cflag |= (CS8);

    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) perror("tcsetattr");
}


// --- Manejo pantalla de terminal ----

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

// Libera toda la lista enlazada (incluyendo texto_resaltado si existe)
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

    strcpy(E.bg_color, "40"); // Fondo negro
    strcpy(E.fg_color, "37"); // Texto blanco

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


void initEditor() {

    E.cx = 0; E.cy = 0;
    E.rowoff = 0; E.coloff = 0;
    E.filas_totales = 0;

    E.primer_linea = NULL;
    E.linea_actual = NULL;

    E.running = 1;

    pthread_mutex_init(&E.mutex_buffer, NULL);

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        // valores de respaldo si ioctl falla (p.ej. redirección de stdout)
        E.screenrows = 24;
        E.screencols = 80;
    }

    E.screenrows -= 2; // Reservar espacio para cabecera y footer

    load_config();

    // El FIFO se crea UNA sola vez aquí, no en cada pulsación de tecla.
    if (mkfifo(FIFO_PATH, 0644) == -1 && errno != EEXIST) {
        perror("mkfifo");
    }
}


// --- Guardado real (usado por autosave_thread y por Ctrl+S) ---

void editor_save_to_disk(void) {

    FILE *out = fopen(E.filename, "w"); // <- corregido: era &E.filename (tipo incorrecto)

    if (out) {
        Linea *tmp = E.primer_linea;
        // Corregido: recorre TODAS las líneas, incluida la última.
        // La versión anterior usaba "while (tmp->siguiente != NULL)", lo
        // cual (a) se saltaba la última línea del archivo y (b) provocaba
        // un acceso a memoria inválida si la lista estaba vacía (tmp == NULL).
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


// --- Resaltado de sintaxis ---
//
// Palabras clave según la extensión del archivo. Se agregan códigos ANSI
// (color) alrededor de cada coincidencia y el resultado se guarda en
// texto_resaltado, dejando texto intacto (el buffer "real" que se edita
// y se guarda en disco nunca se toca).

static const char *KEYWORDS_C[] = {
    "int", "char", "void", "return", "if", "else", "for", "while",
    "struct", "typedef", "const", "static", "include", "define", NULL
};

static const char *KEYWORDS_PY[] = {
    "def", "return", "if", "elif", "else", "for", "while", "import",
    "class", "None", "True", "False", "print", NULL
};

// Devuelve el arreglo de palabras clave apropiado según la extensión
// de E.filename, o NULL si no se reconoce la extensión.
static const char **get_keywords_for_filename(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (ext == NULL) return NULL;
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0) return KEYWORDS_C;
    if (strcmp(ext, ".py") == 0) return KEYWORDS_PY;
    return NULL;
}

#define COLOR_KEYWORD "\x1b[36m" // Cian
#define COLOR_RESET   "\x1b[0m"

// Aplica resaltado a una sola línea usando regex.h (regcomp/regexec),
// buscando coincidencias de palabra completa (\b...\b) para cada keyword.
// Escribe el resultado (con códigos ANSI insertados) en linea->texto_resaltado.
static void resaltar_linea(Linea *linea, const char **keywords) {

    free(linea->texto_resaltado);
    linea->texto_resaltado = NULL;

    if (keywords == NULL || linea->longitud == 0) return;

    // Buffer de salida: en el peor caso cada carácter podría quedar rodeado
    // de códigos ANSI, así que reservamos margen amplio.
    size_t cap = (size_t)linea->longitud * 2 + 256;
    char *salida = malloc(cap);
    salida[0] = '\0';
    size_t usado = 0;

    const char *cursor = linea->texto;

    while (*cursor != '\0') {

        int mejor_inicio = -1, mejor_fin = -1;

        // Probar cada palabra clave en la posición actual del cursor
        for (int k = 0; keywords[k] != NULL; k++) {

            regex_t regex;
            char patron[64];
            snprintf(patron, sizeof(patron), "\\b%s\\b", keywords[k]);

            if (regcomp(&regex, patron, REG_EXTENDED) != 0) continue;

            regmatch_t m;
            if (regexec(&regex, cursor, 1, &m, 0) == 0 && m.rm_so == 0) {
                mejor_inicio = (int)m.rm_so;
                mejor_fin = (int)m.rm_eo;
                regfree(&regex);
                break; // coincide exactamente al inicio del cursor
            }
            regfree(&regex);
        }

        if (mejor_inicio == 0) {
            int len_match = mejor_fin - mejor_inicio;

            size_t necesario = usado + strlen(COLOR_KEYWORD) + (size_t)len_match + strlen(COLOR_RESET) + 1;
            if (necesario > cap) {
                cap = necesario * 2;
                salida = realloc(salida, cap);
            }

            usado += (size_t)snprintf(salida + usado, cap - usado, "%s%.*s%s",
                                       COLOR_KEYWORD, len_match, cursor, COLOR_RESET);
            cursor += len_match;
        } else {
            // Copiar un carácter normal y avanzar
            if (usado + 2 > cap) {
                cap *= 2;
                salida = realloc(salida, cap);
            }
            salida[usado++] = *cursor;
            salida[usado] = '\0';
            cursor++;
        }
    }

    linea->texto_resaltado = salida;
}

void *syntax_thread(void *arg) {
    (void)arg;

    while (E.running) {
        sleep(2);

        pthread_mutex_lock(&E.mutex_buffer);

        const char **keywords = get_keywords_for_filename(E.filename);

        if (keywords != NULL) {
            Linea *actual = E.primer_linea;
            while (actual != NULL) {
                resaltar_linea(actual, keywords);
                actual = actual->siguiente;
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
    printf("\x1b[7m"); // Video inverso

    if (msg != NULL) {
        printf("%-80s", msg);
    } else {
        printf(" ^O Abrir | ^S Guardar | ^A G. Como | ^C Copiar | ^V Pegar | ^X Cortar | ^Q Salir ");
    }

    printf("\x1b[m\x1b[K");
}

void editor_refresh_screen() {

    // NOTA: buffer de tamaño fijo (8192). Con archivos grandes y, sobre
    // todo, con texto_resaltado (que puede duplicar el tamaño por los
    // códigos ANSI), esto puede desbordarse. Para un editor "real" habría
    // que medir el tamaño necesario o usar un buffer dinámico que crezca
    // con realloc, igual que se hizo en resaltar_linea().
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

    // O_WRONLY: "copiar" escribe el contenido de la línea actual hacia el FIFO.
    int fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        // No hay ningún lector esperando al otro lado del FIFO todavía.
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

    // O_RDONLY: "pegar" lee del FIFO lo que otro proceso haya escrito.
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


// --- Procesamiento de Entrada ---
void editor_process_keypress() {

    char c = '\0';

    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) perror("read");

    if (c == '\0') return;

        if (c == '\x1b') {  // Secuencia de escape
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return;

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': E.cy = (E.cy > 0) ? E.cy - 1 : 0; break; // Arriba
                case 'B': E.cy++; break;                          // Abajo
                case 'C': E.cx++; break;                          // Derecha
                case 'D': E.cx = (E.cx > 0) ? E.cx - 1 : 0; break; // Izquierda
                case 'H': E.cx = 0; break;                       // Home
                case 'F': E.cx = (E.linea_actual) ? E.linea_actual->longitud : 0; break; // End
            }
        }
        return;
    }

    // Manejo de Backspace y Delete
    if (c == 127 || c == CTRL_KEY('h')) {   // Backspace
        if (E.linea_actual == NULL) return;
        if (E.cx > 0) {
            // Eliminar carácter antes del cursor
            memmove(&E.linea_actual->texto[E.cx - 1],
                    &E.linea_actual->texto[E.cx],
                    E.linea_actual->longitud - E.cx + 1);
            E.linea_actual->longitud--;
            E.cx--;
        } else {
            // Unir con línea anterior (si existe)
            // (implementación omitida por brevedad)
        }
        return;
    }

    if (c == 127 + 32) { // Delete (en algunas terminales es \x1b[3~, pero lo simplificamos)
        // Eliminar carácter bajo el cursor
        if (E.linea_actual == NULL || E.cx >= E.linea_actual->longitud) return;
        memmove(&E.linea_actual->texto[E.cx],
                &E.linea_actual->texto[E.cx + 1],
                E.linea_actual->longitud - E.cx);
        E.linea_actual->longitud--;
        return;
    }
    switch (c) {

        case CTRL_KEY('q'): // Salir
            editor_shutdown();
            exit(0);
            break;

        case CTRL_KEY('s'): // Guardar
            editor_draw_footer(" Guardando archivo... ");
            fflush(stdout);
            pthread_mutex_lock(&E.mutex_buffer);
            editor_save_to_disk(); // <- ya NO se llama a autosave_thread() directamente
            pthread_mutex_unlock(&E.mutex_buffer);
            break;

        case CTRL_KEY('c'): // Copiar
            accion_copiar();
            break;

        case CTRL_KEY('v'): // Pegar
            accion_pegar();
            break;

        // Movimiento básico (WASD)
        case 'w': E.cy = (E.cy > 0) ? E.cy - 1 : 0; break;
        case 's': E.cy++; break;
        case 'a': E.cx = (E.cx > 0) ? E.cx - 1 : 0; break;
        case 'd': E.cx++; break;

        default:
            if (isprint((unsigned char)c)) {
                insertar_caracter(c);
            }
            break;
    }
}
/*

void editor_process_keypress() {
    char c;
    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) return;

    if (c == '\x1b') {  // Secuencia de escape
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return;

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': E.cy = (E.cy > 0) ? E.cy - 1 : 0; break; // Arriba
                case 'B': E.cy++; break;                          // Abajo
                case 'C': E.cx++; break;                          // Derecha
                case 'D': E.cx = (E.cx > 0) ? E.cx - 1 : 0; break; // Izquierda
                case 'H': E.cx = 0; break;                       // Home
                case 'F': E.cx = (E.linea_actual) ? E.linea_actual->longitud : 0; break; // End
            }
        }
        return;
    }

    // Manejo de Backspace y Delete
    if (c == 127 || c == CTRL_KEY('h')) {   // Backspace
        if (E.linea_actual == NULL) return;
        if (E.cx > 0) {
            // Eliminar carácter antes del cursor
            memmove(&E.linea_actual->texto[E.cx - 1],
                    &E.linea_actual->texto[E.cx],
                    E.linea_actual->longitud - E.cx + 1);
            E.linea_actual->longitud--;
            E.cx--;
        } else {
            // Unir con línea anterior (si existe)
            // (implementación omitida por brevedad)
        }
        return;
    }

    if (c == 127 + 32) { // Delete (en algunas terminales es \x1b[3~, pero lo simplificamos)
        // Eliminar carácter bajo el cursor
        if (E.linea_actual == NULL || E.cx >= E.linea_actual->longitud) return;
        memmove(&E.linea_actual->texto[E.cx],
                &E.linea_actual->texto[E.cx + 1],
                E.linea_actual->longitud - E.cx);
        E.linea_actual->longitud--;
        return;
    }

    // Resto del código (Ctrl+Q, Ctrl+S, etc.)
    switch (c) {
        case CTRL_KEY('q'):  break;
        case CTRL_KEY('s'):  break;
        // ... copiar, pegar, etc.
        default:
            if (isprint((unsigned char)c)) insertar_caracter(c);
            break;
    }
}

*/

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