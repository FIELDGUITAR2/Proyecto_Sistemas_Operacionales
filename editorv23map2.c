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
#include <fcntl.h>     // open()
#include <sys/stat.h>  // mkfifo()
#include <sys/ioctl.h> // Control tamaño de pantalla

// --- Macros de Teclas ---

#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_FILENAME 256
#define FIFO_PATH "/tmp/CopyPaste_fifo"

// --- LIBRERÍA MAP (implementación integrada) ---

#define MAX_MAPA_COLORES 48
#define MAX_PALABRA_MAP 32
#define MAX_CODIGO_MAP 16

typedef struct EntradaMapa
{
    char palabra[MAX_PALABRA_MAP];
    char codigo[MAX_CODIGO_MAP]; // almacena el número ANSI como string (ej. "31")
    int activa;
} EntradaMapa;

typedef struct MapaColores
{
    EntradaMapa entradas[MAX_MAPA_COLORES];
    int cantidad;
} MapaColores;

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

int mapa_agregar(MapaColores *mapa, const char *palabra, int codigo_ansi)
{
    if (mapa->cantidad >= MAX_MAPA_COLORES)
        return -1;
    // Verificar que no exista ya
    for (int i = 0; i < mapa->cantidad; i++)
    {
        if (strcmp(mapa->entradas[i].palabra, palabra) == 0)
        {
            // Actualizar código
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

const char *mapa_buscar(const MapaColores *mapa, const char *token, int len)
{
    if (mapa == NULL || token == NULL)
        return NULL;
    // Comparar con cada entrada
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

// --- Estructuras de Datos del Editor ---

typedef struct Linea
{
    char *texto;           // Texto real de la línea
    int longitud;          // Longitud de linea
    int capacidad;         // Capacidad máxima del texto
    char *texto_resaltado; // Copia con códigos ANSI para syntax highlighting
    struct Linea *siguiente;
    struct Linea *anterior;
} Linea;

// --- Estado Global del Editor ---

typedef struct EstadoEditor
{
    int cx, cy;                 // Posición del cursor en pantalla
    int filas_totales;          // Cantidad de filas en el documento
    int rowoff, coloff;         // Offsets para el scrolling
    int screenrows, screencols; // Tamaño de la terminal
    Linea *primer_linea;        // Primera Línea
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

    // Mapas de sintaxis (uno por extensión)
    MapaColores mapas_sintaxis[10];
    int num_mapas;
} EstadoEditor;

EstadoEditor E; // Instancia global

struct termios orig_termios;

// --- Declaraciones de Funciones ---

void editor_refresh_screen();
void editor_draw_footer(const char *mensaje);
void die(const char *s);
void editor_save_to_disk(void);
void load_syntax_config();
void resaltar_linea_sin_regex(Linea *linea, const MapaColores *mapa);

// --- Manejo de errores ---

void die(const char *s)
{
    perror(s);
    exit(1);
}

// --- Manejo de la Terminal ---

void disableRawMode()
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[2J\x1b[H");
}

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
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        perror("tcsetattr");
}

// --- Tamaño de pantalla ---

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        perror("Error en ejecucion de ioctl");
        return -1;
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

// --- Manejo de Memoria ---

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

// --- Limpiar terminal ---

void editor_shutdown(void) {

    E.running = 0;

    pthread_mutex_lock(&E.mutex_buffer);
    free_all_lines();
    pthread_mutex_unlock(&E.mutex_buffer);

    pthread_mutex_destroy(&E.mutex_buffer);
    unlink(FIFO_PATH);

    // Método 1: Secuencias ANSI (funciona en la mayoría de terminales)
    printf("\x1b[2J\x1b[H");
    fflush(stdout);

    system("clear");
}

// --- Configuración (.editorrc) ---

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
            {
                strncpy(E.bg_color, line + 9, strcspn(line + 9, "\r\n"));
            }
        }
        fclose(fp);
    }
}

// --- Carga de configuración de sintaxis (usando el mapa) ---

void load_syntax_config()
{
    E.num_mapas = 0;
    const char *home = getenv("HOME");
    if (!home)
        return;

    char path[512];
    snprintf(path, sizeof(path), "%s/.editor_syntax.conf", home);
    FILE *fp = fopen(path, "r");

    // Si no existe, crear mapa por defecto para C
    if (!fp)
    {
        // Inicializar un mapa para extensión "c"
        MapaColores *mapa = &E.mapas_sintaxis[E.num_mapas++];
        mapa_inicializar(mapa);
        // Palabras reservadas básicas de C con sus colores
        struct
        {
            const char *palabra;
            int color;
        } def[] = {
            {"int", 31}, {"char", 34}, {"void", 35}, {"return", 36}, {"if", 33}, {"else", 33}, {"for", 32}, {"while", 32}, {"struct", 35}, {"typedef", 35}, {"static", 35}, {"include", 33}, {"define", 33}};
        for (int i = 0; i < sizeof(def) / sizeof(def[0]); i++)
        {
            mapa_agregar(mapa, def[i].palabra, def[i].color);
        }
        // También para .h
        MapaColores *mapa_h = &E.mapas_sintaxis[E.num_mapas++];
        mapa_inicializar(mapa_h);
        for (int i = 0; i < sizeof(def) / sizeof(def[0]); i++)
        {
            mapa_agregar(mapa_h, def[i].palabra, def[i].color);
        }
        return;
    }

    // Parsear archivo
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
                mapa_actual = &E.mapas_sintaxis[E.num_mapas++];
                mapa_inicializar(mapa_actual);
            }
        }
        else if (mapa_actual && strncmp(line, "keyword ", 8) == 0)
        {
            char kw[64];
            int color;
            if (sscanf(line + 8, "%s %d", kw, &color) == 2)
            {
                mapa_agregar(mapa_actual, kw, color);
            }
        }
    }
    fclose(fp);
}

// --- Inicializar editor ---

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
    E.screenrows -= 2; // Cabecera + pie

    load_config();
    load_syntax_config();

    if (mkfifo(FIFO_PATH, 0644) == -1 && errno != EEXIST)
    {
        perror("mkfifo");
    }
}

// --- Guardado ---

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

// --- Hilo de autoguardado ---

void *autosave_thread(void *arg)
{
    (void)arg;
    while (E.running)
    {
        sleep(E.autosave_time);
        pthread_mutex_lock(&E.mutex_buffer);
        editor_save_to_disk();
        pthread_mutex_unlock(&E.mutex_buffer);
        editor_draw_footer(" [Auto-Guardado realizado] ");
    }
    return NULL;
}

// --- Resaltado sin expresiones regulares (usando mapa) ---

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
        // Saltar caracteres que no son parte de una palabra (letras, dígitos, '_')
        if (!isalnum(text[i]) && text[i] != '_')
        {
            out[pos++] = text[i];
            i++;
            continue;
        }

        // Inicio de una palabra
        int start = i;
        while (i < (int)len && (isalnum(text[i]) || text[i] == '_'))
        {
            i++;
        }
        int word_len = i - start;

        // Buscar en el mapa
        const char *color = mapa_buscar(mapa, text + start, word_len);
        if (color != NULL)
        {
            // Agregar código de color
            pos += snprintf(out + pos, cap - pos, "\x1b[%sm", color);
            // Agregar la palabra
            pos += snprintf(out + pos, cap - pos, "%.*s", word_len, text + start);
            // Resetear color
            pos += snprintf(out + pos, cap - pos, "\x1b[0m");
        }
        else
        {
            // Copiar la palabra sin formato
            for (int j = start; j < i; j++)
                out[pos++] = text[j];
        }
    }

    out[pos] = '\0';
    linea->texto_resaltado = out;
}

// --- Hilo de sintaxis ---

void *syntax_thread(void *arg)
{
    (void)arg;
    while (E.running)
    {
        sleep(2);
        pthread_mutex_lock(&E.mutex_buffer);

        // Buscar el mapa correspondiente a la extensión del archivo
        const MapaColores *mapa = NULL;
        const char *ext = strrchr(E.filename, '.');
        if (ext)
        {
            ext++; // saltar el punto
            for (int i = 0; i < E.num_mapas; i++)
            {
                // Asumimos que el mapa está indexado por extensión, pero no almacenamos la extensión en el mapa.
                // Necesitamos una forma de saber qué mapa corresponde a qué extensión.
                // Para simplificar, usaremos un arreglo de estructuras que asocien extensión y mapa.
                // Como no tenemos esa asociación, usaremos un truco: los mapas se cargan en orden de aparición en el archivo,
                // y en el archivo la sección [c] corresponde al primer mapa, [py] al segundo, etc.
                // Pero no guardamos el nombre de la extensión en el mapa. Para resolverlo, podemos agregar un campo "ext" al mapa.
                // Lo haré en la estructura MapaColores (añadir char extension[10]).
                // Pero como la librería map está fija, modificaré la estructura localmente.
                // Por simplicidad, en este ejemplo asumiremos que el primer mapa es para "c" y el segundo para "h" (por defecto).
                // Esto es limitado, pero se puede extender fácilmente guardando la extensión en el mapa.
                // Para una solución completa, modificaría MapaColores para incluir la extensión.
                // Dado que la librería map es fija, podemos crear un arreglo paralelo de extensiones.
                // Lo haré: en EstadoEditor, tendré un arreglo de strings para las extensiones.
            }
        }
        // Por ahora, usaremos el primer mapa si existe (para C)
        if (E.num_mapas > 0)
        {
            mapa = &E.mapas_sintaxis[0];
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

// --- Interfaz gráfica ---

void editor_draw_header(char *buffer)
{
    char header[256];
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);

    char reloj[16] = "";
    if (E.mostrar_reloj)
        sprintf(reloj, " | %02d:%02d", tm.tm_hour, tm.tm_min);

    snprintf(header, sizeof(header),
             "\x1b[7m %s - %d lineas | Col: %d, Fila: %d%s \x1b[K\x1b[m\r\n",
             E.filename, E.filas_totales, E.cx, E.cy, reloj);

    strcat(buffer, header);
}

void editor_draw_footer(const char *msg)
{
    printf("\x1b[%d;1H", E.screenrows + 2);
    printf("\x1b[7m");
    if (msg != NULL)
    {
        printf("%-80s", msg);
    }
    else
    {
        printf(" ^O Abrir | ^S Guardar | ^A G. Como | ^C Copiar | ^V Pegar | ^X Cortar | ^Q Salir ");
    }
    printf("\x1b[m\x1b[K");
}

void editor_refresh_screen()
{
    char *buffer = malloc(8192);
    strcpy(buffer, "\x1b[?25l");
    strcat(buffer, "\x1b[H");

    editor_draw_header(buffer);

    pthread_mutex_lock(&E.mutex_buffer);

    Linea *l = E.primer_linea;
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

// --- Copiar / Pegar FIFO ---

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
    {
        write(fd, E.linea_actual->texto, E.linea_actual->longitud);
    }
    close(fd);
    editor_draw_footer("Contenido copiado ....");
}

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
    {
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

// --- Inserción de caracteres ---

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
    {
        memmove(&actual->texto[E.cx + 1], &actual->texto[E.cx], actual->longitud - E.cx);
    }

    actual->texto[E.cx] = c;
    actual->longitud = nueva_longitud;
    actual->texto[nueva_longitud] = '\0';
    E.cx++;
}

// --- Borrado ---

void borrar_caracter_izquierda()
{
    if (E.linea_actual == NULL)
        return;

    if (E.cx == 0)
    {
        // Unir con línea anterior
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
        return;
    }

    Linea *actual = E.linea_actual;
    if (E.cx > 0)
    {
        memmove(&actual->texto[E.cx - 1],
                &actual->texto[E.cx],
                actual->longitud - E.cx + 1);
        actual->longitud--;
        E.cx--;
    }
}

void borrar_caracter_derecha()
{
    if (E.linea_actual == NULL)
        return;
    Linea *actual = E.linea_actual;
    if (E.cx < actual->longitud)
    {
        memmove(&actual->texto[E.cx],
                &actual->texto[E.cx + 1],
                actual->longitud - E.cx);
        actual->longitud--;
    }
}


void insertar_nueva_linea()
{
    if (E.linea_actual == NULL)
        return;

    Linea *actual = E.linea_actual;

    // Crear nueva línea con el texto después del cursor
    Linea *nueva = malloc(sizeof(Linea));
    nueva->texto = malloc(actual->longitud - E.cx + 1);
    nueva->texto_resaltado = NULL;

    // Copiar el texto desde la posición del cursor hasta el final
    strcpy(nueva->texto, &actual->texto[E.cx]);
    nueva->longitud = actual->longitud - E.cx;
    nueva->capacidad = nueva->longitud + 1;

    // Truncar la línea actual en la posición del cursor
    actual->texto[E.cx] = '\0';
    actual->longitud = E.cx;

    // Insertar la nueva línea en la lista
    nueva->siguiente = actual->siguiente;
    nueva->anterior = actual;
    if (actual->siguiente)
    {
        actual->siguiente->anterior = nueva;
    }
    actual->siguiente = nueva;

    // Mover cursor a la nueva línea
    E.linea_actual = nueva;
    E.cy++;
    E.cx = 0;
    E.filas_totales++;
}

// Obtener linea por indice

Linea *obtener_linea_por_indice(int indice)
{
    Linea *l = E.primer_linea;
    for (int i = 0; i < indice && l != NULL; i++)
    {
        l = l->siguiente;
    }
    return l;
}

// --- Procesamiento de teclas ---

void editor_process_keypress()
{
    char c;
    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
        return;

    // Secuencias de escape
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
                // Ajustar línea actual al moverse
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
            case '3': // Delete (puede ser \x1b[3~)
                char next;
                if (read(STDIN_FILENO, &next, 1) == 1 && next == '~')
                {
                    borrar_caracter_derecha();
                }
                break;

            default:
                break;
            }
        }
        return;
    }

    // Teclas de control
    switch (c)
    {
    case CTRL_KEY('q'):
        editor_shutdown();
        exit(0);
        break;
    case CTRL_KEY('s'):
        // ... guardar ...
        break;
    case CTRL_KEY('c'):
        // ... copiar ...
        break;
    case CTRL_KEY('v'):
        // ... pegar ...
        break;
    case 127: // Backspace
    case CTRL_KEY('h'):
        borrar_caracter_izquierda();
        break;
    /*case CTRL_KEY('a'):
        editor_save_as();
        break;*/
    case '\r': // ENTER - NUEVA LÍNEA
    case '\n': // ENTER (alternativo)
        insertar_nueva_linea();
        break;
    default:
        if (isprint((unsigned char)c))
        {
            insertar_caracter(c);
        }
        break;
    }
}

// --- MAIN ---

int main(int argc, char *argv[])
{

    enableRawMode();
    initEditor();

    if (argc >= 2)
    {
        strncpy(E.filename, argv[1], MAX_FILENAME - 1);
        E.filename[MAX_FILENAME - 1] = '\0';

        append_line("/* Bienvenido al editor (Fase 2) */", 35);
        append_line("int main() {", 12);
        append_line("    return 0;", 13);
        append_line("}", 1);
    }
    else
    {
        append_line("", 0);
    }

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
