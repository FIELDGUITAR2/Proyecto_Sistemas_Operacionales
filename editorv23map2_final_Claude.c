#define _GNU_SOURCE

#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>     // open()
#include <sys/stat.h>  // mkfifo()
#include <sys/ioctl.h> // tamaño de la terminal
#include <sys/select.h> // select() en limpiar_buffer_entrada()

// =====================================================================
//  MACROS Y CONSTANTES
// =====================================================================

#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_FILENAME 256
#define FIFO_PATH "/tmp/CopyPaste_fifo"

// =====================================================================
//  LIBRERÍA "MAP" (mapa palabra -> color ANSI, para el resaltado de sintaxis)
// =====================================================================

#define MAX_MAPA_COLORES 48
#define MAX_PALABRA_MAP 32
#define MAX_CODIGO_MAP 16

// Una entrada del mapa: una palabra reservada y el código de color ANSI que le corresponde.
typedef struct EntradaMapa
{
    char palabra[MAX_PALABRA_MAP];
    char codigo[MAX_CODIGO_MAP]; // código ANSI guardado como texto (ej. "31")
    int activa;
} EntradaMapa;

// Conjunto de palabras reservadas + color, típicamente uno por extensión de archivo (.c, .py, etc).
typedef struct MapaColores
{
    EntradaMapa entradas[MAX_MAPA_COLORES];
    int cantidad;
} MapaColores;

// Deja un MapaColores vacío y listo para usarse.
void mapa_inicializar(MapaColores *mapa)
{
    mapa->cantidad = 0;
    for (int i = 0; i < MAX_MAPA_COLORES; i++)
    {
        mapa->entradas[i].activa = 0;
        mapa->entradas[i].palabra[0] = '\0';
        mapa->entradas[i].codigo[0] = '\0';
    }
}

// Agrega (o actualiza, si ya existe) una palabra con su color ANSI dentro del mapa.
int mapa_agregar(MapaColores *mapa, const char *palabra, int codigo_ansi)
{
    if (mapa->cantidad >= MAX_MAPA_COLORES)
        return -1;

    for (int i = 0; i < mapa->cantidad; i++)
    {
        if (strcmp(mapa->entradas[i].palabra, palabra) == 0)
        {
            snprintf(mapa->entradas[i].codigo, MAX_CODIGO_MAP, "%d", codigo_ansi);
            return 0;
        }
    }

    strncpy(mapa->entradas[mapa->cantidad].palabra, palabra, MAX_PALABRA_MAP - 1);
    mapa->entradas[mapa->cantidad].palabra[MAX_PALABRA_MAP - 1] = '\0';
    snprintf(mapa->entradas[mapa->cantidad].codigo, MAX_CODIGO_MAP, "%d", codigo_ansi);
    mapa->entradas[mapa->cantidad].activa = 1;
    mapa->cantidad++;
    return 0;
}

// Busca "token" (de longitud "len") dentro del mapa. Devuelve su código ANSI o NULL si no existe.
const char *mapa_buscar(const MapaColores *mapa, const char *token, int len)
{
    if (mapa == NULL || token == NULL)
        return NULL;

    for (int i = 0; i < mapa->cantidad; i++)
    {
        if (strlen(mapa->entradas[i].palabra) == (size_t)len &&
            strncmp(mapa->entradas[i].palabra, token, len) == 0)
        {
            return mapa->entradas[i].codigo;
        }
    }
    return NULL;
}

// =====================================================================
//  ESTRUCTURAS DE DATOS DEL EDITOR
// =====================================================================

// Cada línea de texto del documento es un nodo de una lista doblemente enlazada.
typedef struct Linea
{
    char *texto;           // contenido real de la línea
    int longitud;          // longitud en caracteres
    int capacidad;         // tamaño reservado en memoria para "texto"
    char *texto_resaltado; // copia de "texto" con códigos ANSI de color (o NULL si no se ha resaltado)
    struct Linea *siguiente;
    struct Linea *anterior;
} Linea;

// Estado global del editor: cursor, documento, configuración e hilos.
typedef struct EstadoEditor
{
    int cx, cy;                 // posición del cursor (columna, fila) dentro del documento
    int filas_totales;          // cantidad de líneas del documento
    int rowoff, coloff;         // desplazamiento de scroll (fila/columna superior visible)
    int screenrows, screencols; // tamaño de la terminal
    Linea *primer_linea;
    Linea *linea_actual;
    char filename[MAX_FILENAME];

    // Configuración cargada desde ".editorrc"
    char bg_color[20];
    char fg_color[20];
    int mostrar_reloj;
    int autosave_time;

    // Sincronización entre el hilo principal y los hilos de fondo
    pthread_mutex_t mutex_buffer;
    int running;

    // Mapas de sintaxis (uno por extensión de archivo soportada)
    MapaColores mapas_sintaxis[10];
    char extensiones_mapas[10][20]; // extensión (sin el punto) que corresponde a cada mapa
    int num_mapas;
} EstadoEditor;

EstadoEditor E; // instancia única y global del estado del editor
struct termios orig_termios; // configuración original de la terminal (para restaurarla al salir)

// =====================================================================
//  DECLARACIONES DE FUNCIONES
// =====================================================================

void editor_refresh_screen();
void editor_draw_footer(const char *mensaje);
void die(const char *s);
void editor_save_to_disk(void);
void editor_load_from_disk(void);
void editor_open_file(void);
void accion_cortar(void);
void editor_save_as(void);
void accion_copiar(void);
void accion_pegar(void);
void load_syntax_config();
void resaltar_linea_sin_regex(Linea *linea, const MapaColores *mapa);
Linea *obtener_linea_por_indice(int indice);

// =====================================================================
//  MANEJO DE ERRORES
// =====================================================================

// Imprime un mensaje de error del sistema (perror) y termina el programa.
void die(const char *s)
{
    perror(s);
    exit(1);
}

// =====================================================================
//  MANEJO DE LA TERMINAL (modo "raw")
// =====================================================================

// Restaura la configuración original de la terminal y limpia la pantalla al salir.
// Se registra con atexit() para que se ejecute siempre, incluso si el programa
// termina de forma inesperada.
void disableRawMode()
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[2J\x1b[H");
}

// Pone la terminal en modo "raw": sin eco, sin buffer por línea y sin procesamiento
// especial de señales, para poder leer cada tecla al instante.
void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON |
                     IGNBRK | PARMRK | INLCR | IGNCR);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag &= ~(CSIZE | PARENB);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;  // read() puede devolver 0 si no hay tecla disponible
    raw.c_cc[VTIME] = 1; // espera como máximo 0.1s por una tecla

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        perror("tcsetattr");
}

// Descarta cualquier entrada de teclado pendiente en el buffer del kernel.
// Se usa al iniciar el programa para que teclas de una sesión anterior
// (por ejemplo, una tecla mantenida presionada justo antes de cerrar la terminal)
// no se "reproduzcan" como si el usuario las hubiera presionado ahora.
void limpiar_buffer_entrada()
{
    char c;
    fd_set fds;

    tcflush(STDIN_FILENO, TCIFLUSH);

    while (1)
    {
        struct timeval tv = {0, 0};
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);

        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
            break; // no hay más datos pendientes

        if (read(STDIN_FILENO, &c, 1) <= 0)
            break;
        // el caracter leído se descarta a propósito
    }
}

// Obtiene el tamaño actual de la terminal (filas y columnas).
int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        perror("Error en ejecucion de ioctl");
        return -1;
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

// =====================================================================
//  MANEJO DE MEMORIA DEL DOCUMENTO (lista de líneas)
// =====================================================================

// Crea una nueva línea con el texto "s" (de longitud "len") y la agrega
// al final de la lista de líneas del documento.
void append_line(const char *s, size_t len)
{
    pthread_mutex_lock(&E.mutex_buffer);

    Linea *nueva = malloc(sizeof(Linea));
    nueva->texto = malloc(len + 1);
    memcpy(nueva->texto, s, len);
    nueva->texto[len] = '\0';
    nueva->longitud = len;
    nueva->capacidad = len + 1;
    nueva->texto_resaltado = NULL;
    nueva->siguiente = NULL;

    if (E.primer_linea == NULL)
    {
        nueva->anterior = NULL;
        E.primer_linea = nueva;
        E.linea_actual = nueva;
    }
    else
    {
        Linea *temp = E.primer_linea;
        while (temp->siguiente != NULL)
            temp = temp->siguiente;
        temp->siguiente = nueva;
        nueva->anterior = temp;
    }

    E.filas_totales++;
    pthread_mutex_unlock(&E.mutex_buffer);
}

// Libera toda la lista de líneas del documento (todas las Linea y su texto).
void free_all_lines(void)
{
    Linea *actual = E.primer_linea;
    while (actual != NULL)
    {
        Linea *siguiente = actual->siguiente;
        free(actual->texto);
        free(actual->texto_resaltado);
        free(actual);
        actual = siguiente;
    }
    E.primer_linea = NULL;
    E.linea_actual = NULL;
}

// Recorre la lista de líneas y devuelve la línea en la posición "indice" (0 = primera).
Linea *obtener_linea_por_indice(int indice)
{
    Linea *l = E.primer_linea;
    for (int i = 0; i < indice && l != NULL; i++)
        l = l->siguiente;
    return l;
}

// =====================================================================
//  SALIDA / LIMPIEZA DEL PROGRAMA
// =====================================================================

// Detiene los hilos, libera toda la memoria del documento, elimina el FIFO
// de copiar/pegar y limpia la pantalla. Se llama al presionar Ctrl+Q.
void editor_shutdown(void)
{
    E.running = 0;

    pthread_mutex_lock(&E.mutex_buffer);
    free_all_lines();
    pthread_mutex_unlock(&E.mutex_buffer);

    pthread_mutex_destroy(&E.mutex_buffer);
    unlink(FIFO_PATH);

    printf("\x1b[2J\x1b[H");
    fflush(stdout);
    system("clear");
}

// =====================================================================
//  CONFIGURACIÓN (.editorrc)
// =====================================================================

// Carga valores por defecto y luego, si existe, los sobreescribe con lo indicado
// en el archivo ".editorrc" del directorio actual.
void load_config()
{
    strcpy(E.bg_color, "40");
    strcpy(E.fg_color, "37");
    E.mostrar_reloj = 1;
    E.autosave_time = 15;
    strcpy(E.filename, "sinnombre.txt");

    FILE *fp = fopen(".editorrc", "r");
    if (fp)
    {
        char line[256];
        while (fgets(line, sizeof(line), fp))
        {
            if (strncmp(line, "mostrar_reloj=", 14) == 0)
                E.mostrar_reloj = atoi(line + 14);
            if (strncmp(line, "autoguardado=", 13) == 0)
                E.autosave_time = atoi(line + 13);
            if (strncmp(line, "bg_color=", 9) == 0)
                strncpy(E.bg_color, line + 9, strcspn(line + 9, "\r\n"));
        }
        fclose(fp);
    }
}

// =====================================================================
//  CONFIGURACIÓN DE SINTAXIS (~/.editor_syntax.conf)
// =====================================================================

// Carga los mapas de resaltado de sintaxis. Si el usuario tiene un archivo
// "~/.editor_syntax.conf", lo interpreta (secciones "[ext]" + líneas "keyword palabra color").
// Si no existe, crea un mapa básico de C para las extensiones .c y .h.
void load_syntax_config()
{
    E.num_mapas = 0;
    const char *home = getenv("HOME");
    if (!home)
        return;

    char path[512];
    snprintf(path, sizeof(path), "%s/.editor_syntax.conf", home);
    FILE *fp = fopen(path, "r");

    if (!fp)
    {
        MapaColores *mapa = &E.mapas_sintaxis[E.num_mapas];
        strcpy(E.extensiones_mapas[E.num_mapas], "c");
        E.num_mapas++;
        mapa_inicializar(mapa);

        struct
        {
            const char *palabra;
            int color;
        } def[] = {
            {"int", 31}, {"char", 34}, {"void", 35}, {"return", 36},
            {"if", 33}, {"else", 33}, {"for", 32}, {"while", 32},
            {"struct", 35}, {"typedef", 35}, {"static", 35},
            {"include", 33}, {"define", 33}};

        for (size_t i = 0; i < sizeof(def) / sizeof(def[0]); i++)
            mapa_agregar(mapa, def[i].palabra, def[i].color);

        // Mismo mapa para archivos .h
        MapaColores *mapa_h = &E.mapas_sintaxis[E.num_mapas];
        strcpy(E.extensiones_mapas[E.num_mapas], "h");
        E.num_mapas++;
        mapa_inicializar(mapa_h);
        for (size_t i = 0; i < sizeof(def) / sizeof(def[0]); i++)
            mapa_agregar(mapa_h, def[i].palabra, def[i].color);

        return;
    }

    char line[256];
    MapaColores *mapa_actual = NULL;
    while (fgets(line, sizeof(line), fp))
    {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '[')
        {
            char ext[20];
            if (sscanf(line, "[%[^]]]", ext) == 1)
            {
                if (E.num_mapas >= 10)
                    break;
                mapa_actual = &E.mapas_sintaxis[E.num_mapas];
                strncpy(E.extensiones_mapas[E.num_mapas], ext, sizeof(E.extensiones_mapas[0]) - 1);
                E.extensiones_mapas[E.num_mapas][sizeof(E.extensiones_mapas[0]) - 1] = '\0';
                E.num_mapas++;
                mapa_inicializar(mapa_actual);
            }
        }
        else if (mapa_actual && strncmp(line, "keyword ", 8) == 0)
        {
            char kw[64];
            int color;
            if (sscanf(line + 8, "%s %d", kw, &color) == 2)
                mapa_agregar(mapa_actual, kw, color);
        }
    }
    fclose(fp);
}

// =====================================================================
//  INICIALIZACIÓN DEL EDITOR
// =====================================================================

// Inicializa el estado global: cursor en (0,0), tamaño de pantalla, configuración,
// mapas de sintaxis y el FIFO usado para copiar/pegar.
void initEditor()
{
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.filas_totales = 0;
    E.primer_linea = NULL;
    E.linea_actual = NULL;
    E.running = 1;
    E.num_mapas = 0;

    pthread_mutex_init(&E.mutex_buffer, NULL);

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    {
        E.screenrows = 24;
        E.screencols = 80;
    }
    E.screenrows -= 2; // reservar filas para la cabecera y el pie de página

    load_config();
    load_syntax_config();

    if (mkfifo(FIFO_PATH, 0644) == -1 && errno != EEXIST)
        perror("mkfifo");
}

// =====================================================================
//  GUARDADO EN DISCO
// =====================================================================

// Escribe todo el documento (todas las líneas, cada una seguida de '\n') en
// el archivo indicado por E.filename, sobrescribiéndolo por completo.
// El llamador es responsable de tomar el mutex antes de invocar esta función.
void editor_save_to_disk(void)
{
    FILE *out = fopen(E.filename, "w");
    if (out)
    {
        Linea *tmp = E.primer_linea;
        while (tmp != NULL)
        {
            fwrite(tmp->texto, sizeof(char), tmp->longitud, out);
            fputc('\n', out);
            tmp = tmp->siguiente;
        }
        fclose(out);
    }
    else
    {
        perror("Error en fopen");
    }
}

// Lee el archivo indicado por E.filename (si existe) y carga su contenido línea
// por línea en el documento. Se usa al iniciar el editor para no perder lo que
// ya estaba guardado. Si el archivo no existe todavía, empieza con un documento vacío.
void editor_load_from_disk(void)
{
    FILE *fp = fopen(E.filename, "r");
    if (!fp)
    {
        if (E.primer_linea == NULL)
            append_line("", 0);
        return;
    }

    char linea[4096];
    while (fgets(linea, sizeof(linea), fp))
    {
        size_t len = strcspn(linea, "\r\n");
        append_line(linea, len);
    }
    fclose(fp);

    if (E.primer_linea == NULL) // el archivo existía pero estaba vacío
        append_line("", 0);
}

// Hilo en segundo plano que guarda el documento automáticamente cada
// "E.autosave_time" segundos mientras el editor esté en ejecución.
void *autosave_thread(void *arg)
{
    (void)arg;
    while (E.running)
    {
        sleep(E.autosave_time);
        if (!E.running) // evita usar el mutex si editor_shutdown() ya lo destruyó
            break;
        pthread_mutex_lock(&E.mutex_buffer);
        editor_save_to_disk();
        pthread_mutex_unlock(&E.mutex_buffer);
        editor_draw_footer(" [Auto-Guardado realizado] ");
    }
    return NULL;
}

// =====================================================================
//  RESALTADO DE SINTAXIS
// =====================================================================

// Genera "texto_resaltado" a partir de "texto", envolviendo cada palabra
// reservada encontrada en el mapa con sus códigos de color ANSI.
void resaltar_linea_sin_regex(Linea *linea, const MapaColores *mapa)
{
    free(linea->texto_resaltado);
    linea->texto_resaltado = NULL;

    if (mapa == NULL || mapa->cantidad == 0 || linea->longitud == 0)
        return;

    const char *text = linea->texto;
    size_t len = linea->longitud;
    size_t cap = len * 2 + 256;
    char *out = malloc(cap);
    size_t pos = 0;

    int i = 0;
    while (i < (int)len)
    {
        if (!isalnum((unsigned char)text[i]) && text[i] != '_')
        {
            out[pos++] = text[i];
            i++;
            continue;
        }

        int start = i;
        while (i < (int)len && (isalnum((unsigned char)text[i]) || text[i] == '_'))
            i++;
        int word_len = i - start;

        const char *color = mapa_buscar(mapa, text + start, word_len);
        if (color != NULL)
        {
            pos += snprintf(out + pos, cap - pos, "\x1b[%sm", color);
            pos += snprintf(out + pos, cap - pos, "%.*s", word_len, text + start);
            pos += snprintf(out + pos, cap - pos, "\x1b[0m");
        }
        else
        {
            for (int j = start; j < i; j++)
                out[pos++] = text[j];
        }
    }

    out[pos] = '\0';
    linea->texto_resaltado = out;
}

// Hilo en segundo plano que, cada 2 segundos, resalta todas las líneas del
// documento usando el mapa de sintaxis que corresponda a la extensión del archivo actual.
void *syntax_thread(void *arg)
{
    (void)arg;
    while (E.running)
    {
        sleep(2);
        if (!E.running)
            break;
        pthread_mutex_lock(&E.mutex_buffer);

        // Elegir el mapa que corresponda a la extensión real del archivo actual
        const MapaColores *mapa = NULL;
        const char *ext = strrchr(E.filename, '.');
        if (ext)
        {
            ext++; // saltar el punto
            for (int i = 0; i < E.num_mapas; i++)
            {
                if (strcmp(E.extensiones_mapas[i], ext) == 0)
                {
                    mapa = &E.mapas_sintaxis[i];
                    break;
                }
            }
        }

        if (mapa)
        {
            Linea *l = E.primer_linea;
            while (l != NULL)
            {
                resaltar_linea_sin_regex(l, mapa);
                l = l->siguiente;
            }
        }
        pthread_mutex_unlock(&E.mutex_buffer);
    }
    return NULL;
}

// =====================================================================
//  INTERFAZ GRÁFICA (dibujado en pantalla)
// =====================================================================

// Construye la línea de cabecera (nombre de archivo, nº de líneas, posición
// del cursor y reloj opcional) y la agrega al buffer de pantalla.
void editor_draw_header(char *buffer)
{
    char header[256];
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);

    char reloj[16] = "";
    if (E.mostrar_reloj)
        snprintf(reloj, sizeof(reloj), " | %02d:%02d", tm.tm_hour, tm.tm_min);

    snprintf(header, sizeof(header),
             "\x1b[7m %.100s - %d lineas | Col: %d, Fila: %d%s \x1b[K\x1b[m\r\n",
             E.filename, E.filas_totales, E.cx, E.cy, reloj);

    strcat(buffer, header);
}

// Dibuja el pie de página: un mensaje puntual ("msg") o, si es NULL, la
// barra de atajos de teclado por defecto.
void editor_draw_footer(const char *msg)
{
    printf("\x1b[%d;1H", E.screenrows + 2);
    printf("\x1b[7m");
    if (msg != NULL)
        printf("%-80s", msg);
    else
        printf(" ^O Abrir | ^S Guardar | ^A G. Como | ^C Copiar | ^V Pegar | ^X Cortar | ^Q Salir ");
    printf("\x1b[m\x1b[K");
    fflush(stdout);
}

// Redibuja toda la pantalla: cabecera, líneas visibles del documento y pie de página,
// y reposiciona el cursor.
void editor_refresh_screen()
{
    // Ajustar el scroll vertical para que el cursor siempre quede dentro de lo visible
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows)
        E.rowoff = E.cy - E.screenrows + 1;
    if (E.rowoff < 0)
        E.rowoff = 0;

    // El tamaño del buffer se calcula según la pantalla real: con un tamaño fijo (antes 8192)
    // una terminal grande o líneas con muchos códigos de color podían desbordarlo.
    size_t cap = (size_t)(E.screenrows + 4) * ((size_t)E.screencols * 8 + 32) + 1024;
    char *buffer = malloc(cap);
    if (buffer == NULL)
        return;
    buffer[0] = '\0';
    strcat(buffer, "\x1b[?25l");
    strcat(buffer, "\x1b[H");

    editor_draw_header(buffer);

    pthread_mutex_lock(&E.mutex_buffer);

    Linea *l = obtener_linea_por_indice(E.rowoff);
    for (int y = 0; y < E.screenrows; y++)
    {
        if (l != NULL)
        {
            const char *a_mostrar = (l->texto_resaltado != NULL) ? l->texto_resaltado : l->texto;
            strncat(buffer, a_mostrar, E.screencols > 0 ? (size_t)E.screencols * 8 : 80);
            l = l->siguiente;
        }
        else
        {
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

// =====================================================================
//  COPIAR / CORTAR / PEGAR (usando un FIFO como portapapeles)
// =====================================================================

// Copia el texto de la línea actual al FIFO de portapapeles.
void accion_copiar(void)
{
    if (E.linea_actual == NULL)
        return;
    int fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (fd == -1)
    {
        editor_draw_footer(" No hay proceso leyendo el portapapeles ");
        return;
    }
    if (E.linea_actual->texto != NULL && E.linea_actual->longitud > 0)
        write(fd, E.linea_actual->texto, E.linea_actual->longitud);
    close(fd);
    editor_draw_footer("Contenido copiado ....");
}

// Lee lo que haya en el FIFO de portapapeles y lo inserta en la posición actual del cursor.
void accion_pegar(void)
{
    if (E.linea_actual == NULL)
        return;
    int fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (fd == -1)
        return;

    char info_buffer[4096];
    ssize_t bytes_leidos = read(fd, info_buffer, sizeof(info_buffer) - 1);
    close(fd);

    if (bytes_leidos <= 0)
        return;
    info_buffer[bytes_leidos] = '\0';

    Linea *actual = E.linea_actual;
    int nueva_longitud = actual->longitud + (int)bytes_leidos;

    if (nueva_longitud >= actual->capacidad)
    {
        int nueva_capacidad = nueva_longitud + 128;
        char *nuevo_texto = realloc(actual->texto, nueva_capacidad);
        if (nuevo_texto == NULL)
            return;
        actual->texto = nuevo_texto;
        actual->capacidad = nueva_capacidad;
    }

    if (E.cx < actual->longitud)
        memmove(&actual->texto[E.cx + bytes_leidos], &actual->texto[E.cx], actual->longitud - E.cx);

    memcpy(&actual->texto[E.cx], info_buffer, bytes_leidos);
    actual->longitud = nueva_longitud;
    actual->texto[nueva_longitud] = '\0';
    E.cx += (int)bytes_leidos;

    editor_draw_footer("Contenido pegado ....");
}

// Abre otro archivo: descarta el documento actual y carga el contenido de
// "nombre" (pidiendo el nombre por teclado, igual que editor_save_as).
void editor_open_file(void)
{
    char nombre[MAX_FILENAME];
    char mensaje[256];
    int i = 0;
    char c;

    tcflush(STDIN_FILENO, TCIFLUSH);
    memset(nombre, 0, sizeof(nombre));
    editor_draw_footer(" Abrir archivo: ");

    while (1)
    {
        if (read(STDIN_FILENO, &c, 1) != 1)
            continue;

        if (c == '\r' || c == '\n')
        {
            if (i > 0)
            {
                nombre[i] = '\0';
                break;
            }
            continue;
        }

        if (c == '\x1b')
        {
            editor_draw_footer(" Apertura cancelada ");
            sleep(1);
            return;
        }

        if (c == 127 || c == CTRL_KEY('h'))
        {
            if (i > 0)
            {
                i--;
                nombre[i] = '\0';
                snprintf(mensaje, sizeof(mensaje), " Abrir archivo: %s", nombre);
                editor_draw_footer(mensaje);
            }
            continue;
        }

        if (isprint((unsigned char)c) && i < MAX_FILENAME - 1)
        {
            nombre[i++] = c;
            nombre[i] = '\0';
            snprintf(mensaje, sizeof(mensaje), " Abrir archivo: %s", nombre);
            editor_draw_footer(mensaje);
        }
    }

    pthread_mutex_lock(&E.mutex_buffer);
    free_all_lines();
    E.filas_totales = 0;
    strncpy(E.filename, nombre, MAX_FILENAME - 1);
    E.filename[MAX_FILENAME - 1] = '\0';
    pthread_mutex_unlock(&E.mutex_buffer);

    editor_load_from_disk(); // internamente hace su propio lock por línea (vía append_line)

    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.linea_actual = E.primer_linea;

    snprintf(mensaje, sizeof(mensaje), " Archivo abierto: %s ", E.filename);
    editor_draw_footer(mensaje);
    sleep(1);
}

// Corta la línea actual: la copia al portapapeles (FIFO) y luego la elimina
// del documento (o la vacía, si es la única línea que queda).
void accion_cortar(void)
{
    if (E.linea_actual == NULL)
        return;

    accion_copiar();

    Linea *actual = E.linea_actual;

    if (actual->anterior == NULL && actual->siguiente == NULL)
    {
        actual->texto[0] = '\0';
        actual->longitud = 0;
        E.cx = 0;
        editor_draw_footer("Línea cortada ....");
        return;
    }

    Linea *prev = actual->anterior;
    Linea *next = actual->siguiente;

    if (prev)
        prev->siguiente = next;
    if (next)
        next->anterior = prev;
    if (E.primer_linea == actual)
        E.primer_linea = next;

    E.linea_actual = (next != NULL) ? next : prev;
    if (prev == NULL)
        E.cy = 0;
    else
        E.cy--;
    E.cx = 0;
    E.filas_totales--;

    free(actual->texto);
    free(actual->texto_resaltado);
    free(actual);

    editor_draw_footer("Línea cortada ....");
}

// =====================================================================
//  EDICIÓN DE TEXTO (insertar / borrar caracteres y líneas)
// =====================================================================

// Inserta el carácter "c" en la posición actual del cursor, dentro de la línea actual.
void insertar_caracter(char c)
{
    if (E.linea_actual == NULL)
        return;

    Linea *actual = E.linea_actual;
    int nueva_longitud = actual->longitud + 1;

    if (nueva_longitud >= actual->capacidad)
    {
        int nueva_capacidad = nueva_longitud + 128;
        char *nuevo_texto = realloc(actual->texto, nueva_capacidad);
        if (nuevo_texto == NULL)
            return;
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

// Borra el carácter a la izquierda del cursor (Backspace). Si el cursor está al
// inicio de la línea, la fusiona con la línea anterior.
void borrar_caracter_izquierda()
{
    if (E.linea_actual == NULL)
        return;

    if (E.cx == 0)
    {
        if (E.linea_actual->anterior == NULL)
            return;

        Linea *prev = E.linea_actual->anterior;
        int prev_len = prev->longitud;
        int curr_len = E.linea_actual->longitud;

        int new_len = prev_len + curr_len + 1;
        char *new_text = realloc(prev->texto, new_len);
        if (new_text == NULL)
            return;

        memcpy(new_text + prev_len, E.linea_actual->texto, curr_len + 1);
        prev->texto = new_text;
        prev->longitud = new_len - 1;
        prev->capacidad = new_len;

        prev->siguiente = E.linea_actual->siguiente;
        if (E.linea_actual->siguiente)
            E.linea_actual->siguiente->anterior = prev;

        free(E.linea_actual->texto);
        free(E.linea_actual->texto_resaltado);
        free(E.linea_actual);

        E.linea_actual = prev;
        E.filas_totales--;
        E.cx = prev_len;
        E.cy--;
        return;
    }

    Linea *actual = E.linea_actual;
    memmove(&actual->texto[E.cx - 1], &actual->texto[E.cx], actual->longitud - E.cx + 1);
    actual->longitud--;
    E.cx--;
}

// Borra el carácter a la derecha del cursor (tecla Delete / Supr).
void borrar_caracter_derecha()
{
    if (E.linea_actual == NULL)
        return;
    Linea *actual = E.linea_actual;
    if (E.cx < actual->longitud)
    {
        memmove(&actual->texto[E.cx], &actual->texto[E.cx + 1], actual->longitud - E.cx);
        actual->longitud--;
    }
}

// Inserta un salto de línea (Enter): parte la línea actual en el cursor y
// mueve el resto del texto a una nueva línea justo debajo.
void insertar_nueva_linea()
{
    if (E.linea_actual == NULL)
        return;

    Linea *actual = E.linea_actual;

    Linea *nueva = malloc(sizeof(Linea));
    nueva->texto = malloc(actual->longitud - E.cx + 1);
    nueva->texto_resaltado = NULL;

    strcpy(nueva->texto, &actual->texto[E.cx]);
    nueva->longitud = actual->longitud - E.cx;
    nueva->capacidad = nueva->longitud + 1;

    actual->texto[E.cx] = '\0';
    actual->longitud = E.cx;

    nueva->siguiente = actual->siguiente;
    nueva->anterior = actual;
    if (actual->siguiente)
        actual->siguiente->anterior = nueva;
    actual->siguiente = nueva;

    E.linea_actual = nueva;
    E.cy++;
    E.cx = 0;
    E.filas_totales++;
}

// =====================================================================
//  GUARDAR COMO (Ctrl+A)
// =====================================================================

// Pide al usuario un nombre de archivo por teclado, pregunta antes de
// sobrescribir si ya existe, y guarda el documento completo con ese nombre.
//
// Correcciones aplicadas respecto a la versión anterior:
//   1) Antes se llamaba a editor_draw_footer(mensaje) seguido de editor_refresh_screen().
//      Como editor_refresh_screen() vuelve a dibujar el pie de página por defecto al final
//      (editor_draw_footer(NULL)), el mensaje "Guardar como: ..." quedaba tapado de inmediato
//      y el usuario escribía "a ciegas", sin ver lo que iba tecleando.
//      -> Ahora se dibuja el pie de página directamente (sin refrescar toda la pantalla),
//         así el mensaje permanece visible mientras se escribe el nombre.
//   2) El guardado final no protegía el acceso a la lista de líneas con el mutex,
//      pudiendo chocar con el hilo de autoguardado.
//      -> Ahora se usa pthread_mutex_lock/unlock alrededor de editor_save_to_disk().
//   3) La confirmación de sobrescritura leía UN solo byte con read(). Como la terminal
//      está configurada con VMIN=0/VTIME=1, read() puede devolver 0 (ninguna tecla en
//      ese instante) en vez de bloquear esperando al usuario, y el código lo trataba
//      como si el usuario hubiera cancelado.
//      -> Ahora se reintenta la lectura hasta recibir realmente un byte.
void editor_save_as(void)
{
    char nuevo_nombre[MAX_FILENAME];
    char mensaje[256];
    int i = 0;
    char c;

    tcflush(STDIN_FILENO, TCIFLUSH);

    memset(nuevo_nombre, 0, sizeof(nuevo_nombre));
    snprintf(mensaje, sizeof(mensaje), " Guardar como: %s", nuevo_nombre);
    editor_draw_footer(mensaje);

    // Leer el nuevo nombre de archivo carácter por carácter hasta ENTER o ESC
    while (1)
    {
        if (read(STDIN_FILENO, &c, 1) != 1)
            continue; // sin tecla todavía (timeout de VTIME): seguir esperando

        if (c == '\r' || c == '\n')
        {
            if (i > 0)
            {
                nuevo_nombre[i] = '\0';
                break;
            }
            continue; // no se permite guardar con nombre vacío
        }

        if (c == '\x1b')
        {
            editor_draw_footer(" Operación cancelada ");
            sleep(1);
            return;
        }

        if (c == 127 || c == CTRL_KEY('h'))
        {
            if (i > 0)
            {
                i--;
                nuevo_nombre[i] = '\0';
                snprintf(mensaje, sizeof(mensaje), " Guardar como: %s", nuevo_nombre);
                editor_draw_footer(mensaje);
            }
            continue;
        }

        if (isprint((unsigned char)c) && i < MAX_FILENAME - 1)
        {
            nuevo_nombre[i++] = c;
            nuevo_nombre[i] = '\0';
            snprintf(mensaje, sizeof(mensaje), " Guardar como: %s", nuevo_nombre);
            editor_draw_footer(mensaje);
        }
    }

    // Si el archivo ya existe, confirmar antes de sobrescribirlo
    if (access(nuevo_nombre, F_OK) == 0)
    {
        snprintf(mensaje, sizeof(mensaje), " ¿Sobrescribir %.200s? (s/N): ", nuevo_nombre);
        editor_draw_footer(mensaje);

        char respuesta = 0;
        while (read(STDIN_FILENO, &respuesta, 1) != 1)
            continue; // esperar de verdad a que el usuario responda

        if (respuesta != 's' && respuesta != 'S')
        {
            editor_draw_footer(" Guardado cancelado ");
            sleep(1);
            return;
        }
    }

    strncpy(E.filename, nuevo_nombre, MAX_FILENAME - 1);
    E.filename[MAX_FILENAME - 1] = '\0';

    pthread_mutex_lock(&E.mutex_buffer);
    editor_save_to_disk();
    pthread_mutex_unlock(&E.mutex_buffer);

    snprintf(mensaje, sizeof(mensaje), " Archivo guardado como: %s ", E.filename);
    editor_draw_footer(mensaje);
    sleep(1);
}

// =====================================================================
//  PROCESAMIENTO DE TECLAS
// =====================================================================

// Lee una tecla de la entrada estándar y ejecuta la acción correspondiente
// (movimiento de cursor, edición de texto o un atajo de teclado).
void editor_process_keypress()
{
    char c;
    ssize_t nread = read(STDIN_FILENO, &c, 1);

    if (nread == 0)
        return; // no llegó ninguna tecla nueva (timeout de VTIME): no hacer nada
    if (nread == -1)
    {
        if (errno != EAGAIN)
            die("read");
        return;
    }

    // Secuencias de escape (flechas, Home/End, Delete)
    if (c == '\x1b')
    {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return;
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return;

        if (seq[0] == '[')
        {
            switch (seq[1])
            {
            case 'A': // Arriba
                E.cy = (E.cy > 0) ? E.cy - 1 : 0;
                E.linea_actual = obtener_linea_por_indice(E.cy);
                if (E.cx > E.linea_actual->longitud)
                    E.cx = E.linea_actual->longitud;
                break;
            case 'B': // Abajo
                E.cy++;
                if (E.cy >= E.filas_totales)
                    E.cy = E.filas_totales - 1;
                E.linea_actual = obtener_linea_por_indice(E.cy);
                if (E.cx > E.linea_actual->longitud)
                    E.cx = E.linea_actual->longitud;
                break;
            case 'C': // Derecha
                if (E.linea_actual && E.cx < E.linea_actual->longitud)
                    E.cx++;
                break;
            case 'D': // Izquierda
                E.cx = (E.cx > 0) ? E.cx - 1 : 0;
                break;
            case 'H': // Home
                E.cx = 0;
                break;
            case 'F': // End
                if (E.linea_actual)
                    E.cx = E.linea_actual->longitud;
                break;
            case '3': // Delete (secuencia \x1b[3~)
            {
                char next;
                if (read(STDIN_FILENO, &next, 1) == 1 && next == '~')
                    borrar_caracter_derecha();
                break;
            }
            default:
                break;
            }
        }
        return;
    }

    // Atajos de teclado (Ctrl + tecla)
    switch (c)
    {
    case CTRL_KEY('q'):
        editor_shutdown();
        exit(0);
        break;
    case CTRL_KEY('s'): // Guardar
        pthread_mutex_lock(&E.mutex_buffer);
        editor_save_to_disk();
        pthread_mutex_unlock(&E.mutex_buffer);
        editor_draw_footer(" [Guardado manual realizado] ");
        break;
    case CTRL_KEY('a'): // Guardar como
        editor_save_as();
        break;
    case CTRL_KEY('o'): // Abrir archivo
        editor_open_file();
        break;
    case CTRL_KEY('x'): // Cortar línea
        pthread_mutex_lock(&E.mutex_buffer);
        accion_cortar();
        pthread_mutex_unlock(&E.mutex_buffer);
        break;
    case CTRL_KEY('c'): // Copiar
        pthread_mutex_lock(&E.mutex_buffer);
        accion_copiar();
        pthread_mutex_unlock(&E.mutex_buffer);
        break;
    case CTRL_KEY('v'): // Pegar
        pthread_mutex_lock(&E.mutex_buffer);
        accion_pegar();
        pthread_mutex_unlock(&E.mutex_buffer);
        break;
    case 127: // Backspace
    case CTRL_KEY('h'):
        borrar_caracter_izquierda();
        break;
    case '\r': // Enter
    case '\n':
        insertar_nueva_linea();
        break;
    default:
        if (isprint((unsigned char)c))
            insertar_caracter(c);
        break;
    }
}

// =====================================================================
//  PUNTO DE ENTRADA
// =====================================================================

int main(int argc, char *argv[])
{
    enableRawMode();
    limpiar_buffer_entrada();
    initEditor();

    if (argc >= 2)
    {
        strncpy(E.filename, argv[1], MAX_FILENAME - 1);
        E.filename[MAX_FILENAME - 1] = '\0';
    }
    // Si no se pasó archivo por argumento, E.filename ya trae "sinnombre.txt"
    // (definido en load_config(), llamado dentro de initEditor()).

    editor_load_from_disk(); // carga el contenido real del archivo, si existe

    pthread_t t_save, t_syntax;
    pthread_create(&t_save, NULL, autosave_thread, NULL);
    pthread_create(&t_syntax, NULL, syntax_thread, NULL);

    while (1)
    {
        editor_refresh_screen();
        editor_process_keypress();
    }

    return 0;
}
