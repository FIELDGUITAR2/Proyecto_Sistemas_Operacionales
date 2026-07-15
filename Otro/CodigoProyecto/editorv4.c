#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

// --- Macros de Teclas ---

#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_FILENAME 256
#define MAX_REGLAS 16
#define MAX_PALABRAS_REGLA 24
#define MAX_EXTENSIONES 8
#define ARCHIVO_COLORES "colores_c.txt"
#define FIFO_PATH "/tmp/CopyPaste_fifo"

#define KEY_UP    300
#define KEY_DOWN  301
#define KEY_RIGHT 302
#define KEY_LEFT  303
#define KEY_SLEFT  (1000 + 'D')
#define KEY_SRIGHT (1000 + 'C')

#define COLOR_RESET "\x1b[0m"

// --- Estructuras de Datos ---

typedef struct Linea {
    char *texto;
    int longitud;
    int capacidad;
    char *texto_resaltado;
    struct Linea *siguiente;
    struct Linea *anterior;
} Linea;

typedef enum {
    REGLA_PALABRAS,
    REGLA_LITERAL,
    REGLA_COMENTARIO
} TipoRegla;

typedef struct ReglaColor {
    TipoRegla tipo;
    char ansi[16];
    char palabras[MAX_PALABRAS_REGLA][32];
    int num_palabras;
    char literal[4];
} ReglaColor;

typedef struct ConfigSintaxis {
    char nombre[16];
    char extensiones[MAX_EXTENSIONES][8];
    int num_extensiones;
    ReglaColor reglas[MAX_REGLAS];
    int num_reglas;
    int cargada;
} ConfigSintaxis;

typedef struct EstadoEditor {
    int cx, cy;
    int filas_totales;
    int rowoff, coloff;
    int screenrows, screencols;
    Linea *primer_linea;
    Linea *linea_actual;
    char filename[MAX_FILENAME];

    char bg_color[20];
    char fg_color[20];
    int mostrar_reloj;
    int autosave_time;

    int sel_activa;
    int sel_inicio_fila, sel_inicio_col;
    int sel_fin_fila, sel_fin_col;

    pthread_mutex_t mutex_buffer;
    int running;
} EstadoEditor;

EstadoEditor E;
struct termios orig_termios;

static ConfigSintaxis config_sintaxis;
static ConfigSintaxis *config_activa = NULL;

// --- Declaraciones ---

void die(const char *s);
void editor_refresh_screen(void);
void editor_draw_footer(const char *mensaje);
void editor_save_to_disk(void);
void editor_shutdown(void);

// --- Utilidades ---

void die(const char *s) {
    perror(s);
    exit(1);
}

// --- Terminal ---

void disableRawMode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[2J\x1b[H");
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = orig_termios;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON |
                      IGNBRK | PARMRK | INLCR | IGNCR);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) perror("tcsetattr");
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        perror("ioctl");
        return -1;
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

// --- Parseo rudimentario de colores_c.txt ---

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *fin = s + strlen(s);
    while (fin > s && (fin[-1] == ' ' || fin[-1] == '\t' || fin[-1] == '\r' || fin[-1] == '\n'))
        fin--;
    *fin = '\0';
    return s;
}

static int extraer_cadena_entre_comillas(const char *linea, char *dest, size_t dest_len) {
    const char *ini = strchr(linea, '"');
    if (!ini) return -1;
    ini++;
    const char *fin = strchr(ini, '"');
    if (!fin) return -1;

    size_t len = (size_t)(fin - ini);
    if (len >= dest_len) len = dest_len - 1;
    memcpy(dest, ini, len);
    dest[len] = '\0';
    return 0;
}

static void parsear_extensiones(const char *patron, ConfigSintaxis *cfg) {
    const char *ini = strchr(patron, '(');
    const char *fin = strchr(patron, ')');
    if (!ini || !fin || fin <= ini) return;

    char buffer[64];
    size_t len = (size_t)(fin - ini - 1);
    if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
    memcpy(buffer, ini + 1, len);
    buffer[len] = '\0';

    char *tok = strtok(buffer, "|");
    while (tok && cfg->num_extensiones < MAX_EXTENSIONES) {
        snprintf(cfg->extensiones[cfg->num_extensiones], 8, ".%s", tok);
        cfg->num_extensiones++;
        tok = strtok(NULL, "|");
    }
}

static void parsear_palabras_alternadas(const char *patron, ReglaColor *regla) {
    const char *ini = strchr(patron, '(');
    const char *fin = strrchr(patron, ')');
    if (!ini || !fin || fin <= ini) return;

    char buffer[256];
    size_t len = (size_t)(fin - ini - 1);
    if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
    memcpy(buffer, ini + 1, len);
    buffer[len] = '\0';

    char *tok = strtok(buffer, "|");
    while (tok && regla->num_palabras < MAX_PALABRAS_REGLA) {
        strncpy(regla->palabras[regla->num_palabras], tok, 31);
        regla->num_palabras++;
        tok = strtok(NULL, "|");
    }
}

static void clasificar_patron(const char *patron, int codigo_color, ConfigSintaxis *cfg) {
    if (cfg->num_reglas >= MAX_REGLAS) return;

    ReglaColor *regla = &cfg->reglas[cfg->num_reglas];
    memset(regla, 0, sizeof(*regla));
    snprintf(regla->ansi, sizeof(regla->ansi), "\x1b[%dm", codigo_color);

    if (strncmp(patron, "//", 2) == 0) {
        regla->tipo = REGLA_COMENTARIO;
        cfg->num_reglas++;
        return;
    }

    if (strcmp(patron, "\\.") == 0 || strcmp(patron, ".") == 0 ||
        strcmp(patron, "\".\"") == 0) {
        regla->tipo = REGLA_LITERAL;
        regla->literal[0] = '.';
        cfg->num_reglas++;
        return;
    }

    if (strchr(patron, '(') != NULL && strchr(patron, '|') != NULL) {
        regla->tipo = REGLA_PALABRAS;
        parsear_palabras_alternadas(patron, regla);
        if (regla->num_palabras > 0)
            cfg->num_reglas++;
        return;
    }

    if (strncmp(patron, "\\b", 2) == 0) {
        regla->tipo = REGLA_PALABRAS;
        char palabra[32];
        const char *p = patron + 2;
        if (*p == '(') {
            parsear_palabras_alternadas(patron, regla);
        } else {
            size_t i = 0;
            while (p[i] && p[i] != '\\' && i < 31) {
                palabra[i] = p[i];
                i++;
            }
            palabra[i] = '\0';
            if (i > 0) {
                strncpy(regla->palabras[0], palabra, 31);
                regla->num_palabras = 1;
            }
        }
        if (regla->num_palabras > 0)
            cfg->num_reglas++;
    }
}

static void load_colores_syntax(void) {
    memset(&config_sintaxis, 0, sizeof(config_sintaxis));

    FILE *fp = fopen(ARCHIVO_COLORES, "r");
    if (!fp) return;

    char linea[256];
    while (fgets(linea, sizeof(linea), fp)) {
        char *l = trim(linea);
        if (l[0] == '#' || l[0] == '\0') continue;

        if (strncmp(l, "syntax", 6) == 0) {
            const char *seg = l;
            int count = 0;
            char tmp[128];

            while (*seg) {
                if (*seg == '"') {
                    count++;
                    const char *ini = seg + 1;
                    const char *fin = strchr(ini, '"');
                    if (!fin) break;
                    size_t len = (size_t)(fin - ini);
                    if (len < sizeof(tmp)) {
                        memcpy(tmp, ini, len);
                        tmp[len] = '\0';
                        if (count == 1)
                            strncpy(config_sintaxis.nombre, tmp,
                                    sizeof(config_sintaxis.nombre) - 1);
                        else if (count == 2)
                            parsear_extensiones(tmp, &config_sintaxis);
                    }
                    seg = fin + 1;
                } else {
                    seg++;
                }
            }
            continue;
        }

        if (strncmp(l, "color", 5) == 0) {
            int codigo = 0;
            char patron[128] = "";

            if (sscanf(l + 5, "%d", &codigo) != 1) continue;
            if (extraer_cadena_entre_comillas(l, patron, sizeof(patron)) != 0) continue;

            clasificar_patron(patron, codigo, &config_sintaxis);
        }
    }

    fclose(fp);
    config_sintaxis.cargada = config_sintaxis.num_reglas > 0;
}

static int extension_coincide(const char *filename, ConfigSintaxis *cfg) {
    if (cfg->num_extensiones == 0) return 0;

    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;

    for (int i = 0; i < cfg->num_extensiones; i++) {
        if (strcmp(ext, cfg->extensiones[i]) == 0)
            return 1;
    }
    return 0;
}

static void activar_config_por_archivo(const char *filename) {
    if (config_sintaxis.cargada && extension_coincide(filename, &config_sintaxis))
        config_activa = &config_sintaxis;
    else
        config_activa = NULL;
}

// --- Memoria de líneas ---

void append_line(const char *s, size_t len) {
    pthread_mutex_lock(&E.mutex_buffer);

    Linea *nueva = malloc(sizeof(Linea));
    nueva->texto = malloc(len + 1);
    memcpy(nueva->texto, s, len);
    nueva->texto[len] = '\0';
    nueva->longitud = (int)len;
    nueva->capacidad = (int)len + 1;
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

static Linea *obtener_linea_por_indice(int indice) {
    Linea *l = E.primer_linea;
    for (int i = 0; i < indice && l != NULL; i++)
        l = l->siguiente;
    return l;
}

static void sincronizar_linea_actual(void) {
    E.linea_actual = obtener_linea_por_indice(E.cy);
    if (E.linea_actual && E.cx > E.linea_actual->longitud)
        E.cx = E.linea_actual->longitud;
}

// --- Configuración e inicialización ---

void load_config(void) {
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
            if (strncmp(line, "bg_color=", 9) == 0)
                strncpy(E.bg_color, line + 9, strcspn(line + 9, "\r\n"));
        }
        fclose(fp);
    }
}

void initEditor(void) {
    E.cx = 0; E.cy = 0;
    E.rowoff = 0; E.coloff = 0;
    E.filas_totales = 0;
    E.primer_linea = NULL;
    E.linea_actual = NULL;
    E.running = 1;

    E.sel_activa = 0;
    E.sel_inicio_fila = E.sel_inicio_col = 0;
    E.sel_fin_fila = E.sel_fin_col = 0;

    pthread_mutex_init(&E.mutex_buffer, NULL);

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        E.screenrows = 24;
        E.screencols = 80;
    }
    E.screenrows -= 2;

    load_config();
    load_colores_syntax();

    if (mkfifo(FIFO_PATH, 0644) == -1 && errno != EEXIST)
        perror("mkfifo");
}

static void cargar_archivo_desde_disco(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char *buf = NULL;
    size_t cap = 0;
    ssize_t n;

    while ((n = getline(&buf, &cap, fp)) != -1) {
        if (n > 0 && buf[n - 1] == '\n') n--;
        append_line(buf, (size_t)n);
    }

    free(buf);
    fclose(fp);
    E.cy = 0;
    E.cx = 0;
    sincronizar_linea_actual();
}

// --- Guardado ---

void editor_save_to_disk(void) {
    FILE *out = fopen(E.filename, "w");
    if (!out) {
        perror("fopen");
        return;
    }

    Linea *tmp = E.primer_linea;
    while (tmp != NULL) {
        fwrite(tmp->texto, sizeof(char), tmp->longitud, out);
        fputc('\n', out);
        tmp = tmp->siguiente;
    }
    fclose(out);
}

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

// --- Resaltado rudimentario (sin regex.h) ---

static int es_limite_izq(const char *texto, int pos) {
    if (pos == 0) return 1;
    char c = texto[pos - 1];
    return !isalnum((unsigned char)c) && c != '_';
}

static int es_limite_der(const char *texto, int pos, int len, int match_len) {
    int next = pos + match_len;
    if (next >= len) return 1;
    char c = texto[next];
    return !isalnum((unsigned char)c) && c != '_';
}

static int buscar_palabra_regla(const char *texto, int pos, int len,
                                ReglaColor *regla, int *match_len) {
    for (int i = 0; i < regla->num_palabras; i++) {
        const char *palabra = regla->palabras[i];
        int plen = (int)strlen(palabra);
        if (pos + plen > len) continue;
        if (memcmp(texto + pos, palabra, (size_t)plen) != 0) continue;
        if (!es_limite_izq(texto, pos)) continue;
        if (!es_limite_der(texto, pos, len, plen)) continue;
        *match_len = plen;
        return 1;
    }
    return 0;
}

static const char *color_comentario(void) {
    for (int i = 0; i < config_activa->num_reglas; i++) {
        if (config_activa->reglas[i].tipo == REGLA_COMENTARIO)
            return config_activa->reglas[i].ansi;
    }
    return "\x1b[36m";
}

static const char *color_literal(char c) {
    for (int i = 0; i < config_activa->num_reglas; i++) {
        ReglaColor *r = &config_activa->reglas[i];
        if (r->tipo == REGLA_LITERAL && r->literal[0] == c)
            return r->ansi;
    }
    return NULL;
}

static const char *color_palabra_en_pos(const char *texto, int pos, int len, int *match_len) {
    *match_len = 0;
    for (int i = 0; i < config_activa->num_reglas; i++) {
        ReglaColor *r = &config_activa->reglas[i];
        if (r->tipo != REGLA_PALABRAS) continue;
        if (buscar_palabra_regla(texto, pos, len, r, match_len))
            return r->ansi;
    }
    return NULL;
}

static void resaltar_linea(Linea *linea) {
    free(linea->texto_resaltado);
    linea->texto_resaltado = NULL;

    if (!config_activa || linea->longitud == 0) return;

    size_t cap = (size_t)linea->longitud * 4 + 256;
    char *salida = malloc(cap);
    size_t usado = 0;
    salida[0] = '\0';

    const char *texto = linea->texto;
    int len = linea->longitud;
    int i = 0;

    while (i < len) {
        if (texto[i] == '/' && i + 1 < len && texto[i + 1] == '/') {
            const char *cc = color_comentario();
            size_t resto = (size_t)(len - i);
            size_t necesario = usado + strlen(cc) + resto + strlen(COLOR_RESET) + 1;
            if (necesario > cap) {
                cap = necesario * 2;
                salida = realloc(salida, cap);
            }
            usado += (size_t)snprintf(salida + usado, cap - usado, "%s%s%s",
                                       cc, texto + i, COLOR_RESET);
            break;
        }

        int match_len = 0;
        const char *color = color_palabra_en_pos(texto, i, len, &match_len);

        if (!color) {
            const char *cl = color_literal(texto[i]);
            if (cl) {
                color = cl;
                match_len = 1;
            }
        }

        if (color && match_len > 0) {
            size_t necesario = usado + strlen(color) + (size_t)match_len + strlen(COLOR_RESET) + 1;
            if (necesario > cap) {
                cap = necesario * 2;
                salida = realloc(salida, cap);
            }
            usado += (size_t)snprintf(salida + usado, cap - usado, "%s%.*s%s",
                                       color, match_len, texto + i, COLOR_RESET);
            i += match_len;
        } else {
            if (usado + 2 > cap) {
                cap *= 2;
                salida = realloc(salida, cap);
            }
            salida[usado++] = texto[i];
            salida[usado] = '\0';
            i++;
        }
    }

    linea->texto_resaltado = salida;
}

void *syntax_thread(void *arg) {
    (void)arg;
    while (E.running) {
        sleep(2);
        pthread_mutex_lock(&E.mutex_buffer);
        if (config_activa) {
            Linea *actual = E.primer_linea;
            while (actual != NULL) {
                resaltar_linea(actual);
                actual = actual->siguiente;
            }
        }
        pthread_mutex_unlock(&E.mutex_buffer);
    }
    return NULL;
}

// --- Interfaz ---

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
        printf("%-*s", E.screencols > 0 ? E.screencols : 80, msg);
    } else {
        printf(" Flechas: mover | Shift+Flechas: seleccionar/copiar | ^H Retroceso | ^S Guardar | ^Q Salir ");
    }
    printf("\x1b[m\x1b[K");
}

void editor_refresh_screen(void) {
    char *buffer = malloc(16384);
    strcpy(buffer, "\x1b[?25l\x1b[H");

    editor_draw_header(buffer);

    pthread_mutex_lock(&E.mutex_buffer);

    for (int y = 0; y < E.screenrows; y++) {
        int fila_real = y + E.rowoff;
        Linea *linea = obtener_linea_por_indice(fila_real);

        if (linea != NULL) {
            const char *a_mostrar = linea->texto_resaltado ? linea->texto_resaltado : linea->texto;
            int inicio = E.coloff;
            if (inicio < linea->longitud) {
                int max_chars = E.screencols > 0 ? E.screencols : 80;
                strncat(buffer, a_mostrar + inicio, (size_t)max_chars);
            }
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

// --- Selección y copiado ---

static void iniciar_seleccion(void) {
    E.sel_activa = 1;
    E.sel_inicio_fila = E.cy;
    E.sel_inicio_col = E.cx;
    E.sel_fin_fila = E.cy;
    E.sel_fin_col = E.cx;
}

static void extender_seleccion(int nueva_fila, int nueva_col) {
    if (!E.sel_activa) iniciar_seleccion();
    E.sel_fin_fila = nueva_fila;
    E.sel_fin_col = nueva_col;
}

static void normalizar_seleccion(int *f1, int *c1, int *f2, int *c2) {
    if (*f1 < *f2 || (*f1 == *f2 && *c1 <= *c2)) return;
    int tf = *f1, tc = *c1;
    *f1 = *f2; *c1 = *c2;
    *f2 = tf; *c2 = tc;
}

static int copiar_seleccion_a_fifo(void) {
    if (!E.sel_activa) return 0;

    int f1 = E.sel_inicio_fila, c1 = E.sel_inicio_col;
    int f2 = E.sel_fin_fila, c2 = E.sel_fin_col;
    normalizar_seleccion(&f1, &c1, &f2, &c2);

    int fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd == -1) return -1;

    if (f1 == f2) {
        Linea *l = obtener_linea_por_indice(f1);
        if (l && c1 < l->longitud) {
            int fin = c2 < l->longitud ? c2 : l->longitud;
            if (fin > c1)
                write(fd, l->texto + c1, (size_t)(fin - c1));
        }
    } else {
        for (int f = f1; f <= f2; f++) {
            Linea *l = obtener_linea_por_indice(f);
            if (!l) continue;
            int inicio = (f == f1) ? c1 : 0;
            int fin = (f == f2) ? (c2 < l->longitud ? c2 : l->longitud) : l->longitud;
            if (fin > inicio)
                write(fd, l->texto + inicio, (size_t)(fin - inicio));
            if (f < f2) write(fd, "\n", 1);
        }
    }

    close(fd);
    return 0;
}

void accion_copiar(void) {
    if (E.sel_activa) {
        if (copiar_seleccion_a_fifo() == 0)
            editor_draw_footer(" Selección copiada al portapapeles ");
        else
            editor_draw_footer(" No hay proceso leyendo el portapapeles ");
        return;
    }

    if (E.linea_actual == NULL) return;

    int fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        editor_draw_footer(" No hay proceso leyendo el portapapeles ");
        return;
    }

    if (E.linea_actual->texto && E.linea_actual->longitud > 0)
        write(fd, E.linea_actual->texto, E.linea_actual->longitud);

    close(fd);
    editor_draw_footer(" Línea copiada al portapapeles ");
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
        if (!nuevo_texto) return;
        actual->texto = nuevo_texto;
        actual->capacidad = nueva_capacidad;
    }

    if (E.cx < actual->longitud)
        memmove(&actual->texto[E.cx + bytes_leidos], &actual->texto[E.cx],
                actual->longitud - E.cx);

    memcpy(&actual->texto[E.cx], info_buffer, bytes_leidos);
    actual->longitud = nueva_longitud;
    actual->texto[nueva_longitud] = '\0';
    E.cx += (int)bytes_leidos;

    editor_draw_footer(" Contenido pegado ");
}

// --- Edición de texto ---

void insertar_caracter(char c) {
    if (E.linea_actual == NULL) return;

    E.sel_activa = 0;

    Linea *actual = E.linea_actual;
    int nueva_longitud = actual->longitud + 1;

    if (nueva_longitud >= actual->capacidad) {
        int nueva_capacidad = nueva_longitud + 128;
        char *nuevo_texto = realloc(actual->texto, nueva_capacidad);
        if (!nuevo_texto) return;
        actual->texto = nuevo_texto;
        actual->capacidad = nueva_capacidad;
    }

    if (E.cx < actual->longitud)
        memmove(&actual->texto[E.cx + 1], &actual->texto[E.cx], actual->longitud - E.cx);

    actual->texto[E.cx] = c;
    actual->longitud = nueva_longitud;
    actual->texto[nueva_longitud] = '\0';
    E.cx++;
}

void borrar_caracter(void) {
    if (E.linea_actual == NULL || E.cx == 0) return;

    E.sel_activa = 0;

    Linea *actual = E.linea_actual;
    memmove(&actual->texto[E.cx - 1], &actual->texto[E.cx],
            actual->longitud - E.cx);
    actual->longitud--;
    actual->texto[actual->longitud] = '\0';
    E.cx--;
}

// --- Navegación con flechas ---

static void mover_arriba(void) {
    if (E.cy > 0) {
        E.cy--;
        sincronizar_linea_actual();
    }
}

static void mover_abajo(void) {
    if (E.cy < E.filas_totales - 1) {
        E.cy++;
        sincronizar_linea_actual();
    }
}

static void mover_izquierda(void) {
    if (E.cx > 0) {
        E.cx--;
    } else if (E.cy > 0) {
        E.cy--;
        sincronizar_linea_actual();
        if (E.linea_actual)
            E.cx = E.linea_actual->longitud;
    }
}

static void mover_derecha(void) {
    if (E.linea_actual && E.cx < E.linea_actual->longitud) {
        E.cx++;
    } else if (E.cy < E.filas_totales - 1) {
        E.cy++;
        sincronizar_linea_actual();
        E.cx = 0;
    }
}

static void mover_con_shift(int dir) {
    int nueva_fila = E.cy;
    int nueva_col = E.cx;

    switch (dir) {
        case 'A': if (nueva_fila > 0) nueva_fila--; break;
        case 'B': if (nueva_fila < E.filas_totales - 1) nueva_fila++; break;
        case 'D': if (nueva_col > 0) nueva_col--;
                  else if (nueva_fila > 0) {
                      nueva_fila--;
                      Linea *l = obtener_linea_por_indice(nueva_fila);
                      nueva_col = l ? l->longitud : 0;
                  }
                  break;
        case 'C':
            {
                Linea *l = obtener_linea_por_indice(nueva_fila);
                if (l && nueva_col < l->longitud) nueva_col++;
                else if (nueva_fila < E.filas_totales - 1) {
                    nueva_fila++;
                    nueva_col = 0;
                }
            }
            break;
    }

    extender_seleccion(nueva_fila, nueva_col);
    E.cy = nueva_fila;
    E.cx = nueva_col;
    sincronizar_linea_actual();

    if (copiar_seleccion_a_fifo() == 0)
        editor_draw_footer(" Shift+Flecha: selección copiada ");
    else
        editor_draw_footer(" Shift+Flecha: selección activa (sin lector FIFO) ");
}

static int read_key(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;

    if (c == '\x1b') {
        char seq[8];
        int n = 0;

        if (read(STDIN_FILENO, &seq[n], 1) != 1) return '\x1b';
        n++;

        if (seq[0] == '[') {
            if (read(STDIN_FILENO, &seq[n], 1) != 1) return '\x1b';
            n++;

            if (seq[1] >= '0' && seq[1] <= '9') {
                while (n < 7) {
                    if (read(STDIN_FILENO, &seq[n], 1) != 1) break;
                    if (seq[n] == 'A' || seq[n] == 'B' ||
                        seq[n] == 'C' || seq[n] == 'D') {
                        if (strstr(seq, ";2"))
                            return 1000 + seq[n];
                        return 2000 + seq[n];
                    }
                    n++;
                }
            }

            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return '\x1b';
    }

    return (unsigned char)c;
}

void editor_shutdown(void) {
    E.running = 0;
    pthread_mutex_lock(&E.mutex_buffer);
    free_all_lines();
    pthread_mutex_unlock(&E.mutex_buffer);
    pthread_mutex_destroy(&E.mutex_buffer);
    unlink(FIFO_PATH);
    printf("\x1b[2J\x1b[H");
}

// --- Procesamiento de entrada ---

void editor_process_keypress(void) {
    int c = read_key();
    if (c == 0) return;

    switch (c) {
        case CTRL_KEY('q'):
            editor_shutdown();
            exit(0);
            break;

        case CTRL_KEY('s'):
            editor_draw_footer(" Guardando archivo... ");
            fflush(stdout);
            pthread_mutex_lock(&E.mutex_buffer);
            editor_save_to_disk();
            pthread_mutex_unlock(&E.mutex_buffer);
            editor_draw_footer(" Archivo guardado ");
            break;

        case CTRL_KEY('c'):
            accion_copiar();
            break;

        case CTRL_KEY('v'):
            accion_pegar();
            break;

        case '\x7f':
        case '\b':
        case CTRL_KEY('h'):
            borrar_caracter();
            break;

        case KEY_UP:
            mover_arriba();
            break;

        case KEY_DOWN:
            mover_abajo();
            break;

        case KEY_LEFT:
            mover_izquierda();
            break;

        case KEY_RIGHT:
            mover_derecha();
            break;

        case KEY_SLEFT:
            mover_con_shift('D');
            break;

        case KEY_SRIGHT:
            mover_con_shift('C');
            break;

        default:
            if (isprint(c))
                insertar_caracter((char)c);
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
        activar_config_por_archivo(E.filename);
        cargar_archivo_desde_disco(argv[1]);
    } else {
        activar_config_por_archivo(E.filename);
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
