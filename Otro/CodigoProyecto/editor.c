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
#include <sys/ioctl.h> //Agregado de control tamaño de pantalla


// --- Macros de Teclas ---

#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_FILENAME 256





// --- Estructuras de Datos (Memoria Dinámica) ---

typedef struct Linea {

    char *texto; //Posicion inicial de texto
    int longitud; //Longitud de linea
    int capacidad; //Capacidad máxima del texto
    struct Linea *siguiente; // Siguiente Línea 
    struct Linea *anterior; // Línea Previa

} Linea;


// --- Estado Global del Editor ---

typedef  EstadoEditor {

    int cx, cy;             // Posición del cursor en pantalla
    int filas_totales;      // Cantidad de filas en el documento
    int rowoff, coloff;     // Offsets para el scrolling
    int screenrows, screencols; // Tamaño de la terminal
    Linea *primer_linea;     //Primer Línea en 
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

} E;


struct termios orig_termios;

// --- Declaraciones de Funciones ---

void editor_refresh_screen();
void editor_draw_footer(const char *mensaje);
// --- Manejo de la Terminal (SO a bajo nivel) ---

void disableRawMode() {

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[2J\x1b[H"); // Limpiar pantalla al salir

}


void enableRawMode() {

    //Guardar atributos de terminakl
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = orig_termios;

    //Check Extra
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr"); 

    // Desactivar flags:

    // BRKINT, ICRNL, INPCK, ISTRIP, IXON (Ctrl+S, Ctrl+Q gestionados manualmente)
    // OPOST (Procesamiento de salida)

    // ECHO, ICANON (Modo raw), IEXTEN, ISIG (Ctrl+C, Ctrl+Z gestionados manualmente)


    //
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON |
                     IGNBRK | PARMRK | INLCR | IGNCR);

    // NO Post-Processing
    raw.c_oflag &= ~(OPOST);

    //NO PARITY OR SIZE CHECK
    raw.c_cfloag &= ~(CSIZE | PARENB);

    //Caracteres de 8bits
    raw.c_cflag |= (CS8);

    //NO Comandos especiales ^Z, ^C entre otros!!!
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 0; // Retorno en pura si es 
    raw.c_cc[VTIME] = 1; // Timeout de 1/10 seg para read

    //Aplicar nuevos atributos a la terminal
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) perror("tcsetattr");


}


// --- Manejo pantalla de terminal ----

int getWindowSize(int *rows, int *cols) {

    //LLamada a la estructura real
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){

        perror("Error en ejecucion de ioctl");
        return -1;

    }else{

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


// --- Configuración e Inicialización ---

void load_config() {

    // Valores por defecto

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

    getWindowSize(&E.screenrows, &E.screencols);

    E.screenrows -= 2; // Reservar espacio para cabecera y footer

    load_config();

}


// --- HILOS (Threads) ---


void *autosave_thread(void *arg) {

    while (E.running) {

        sleep(E.autosave_time);

        // Simulación de guardado
        pthread_mutex_lock(&E.mutex_buffer);

        // ------------------ SECCIÓN DE IMPLEMENTACIÓN  ----------------
        //Aquí los estudiantes deben implementar el fopen y volcado de la lista enlazada
        
        FILE *out = fopen(&E.filename, "w");

        if(out){

            Linea *tmp = E.primer_linea; // Se ubica al inicio de la lista enlazada

                while (tmp->siguiente != NULL){//Hasta que el siguiente NO este vacío ciclo

                    //Corregido, en tamaño de bytes en el segundo parametro usar sizeof
                    fwrite(tmp->texto, sizeof(char), tmp->longitud, out); 
                    //Lee de Linea su Info y de ahi escribe en el Archivo out
                    fputc('\n', out);
                    tmp = tmp->siguiente; //Avanza hacia la siguiente línea en el archivo "local"
                } 

                fclose(out); //Cierra el archivo
                
        }else{

            perror("Error en fopen"); //Debugging
        }
        // ------------------ SECCIÓN DE IMPLEMENTACIÓN ------------------------

        editor_draw_footer(" [Auto-Guardado realizado] ");
        pthread_mutex_unlock(&E.mutex_buffer);

    }

    return NULL;

}


void *syntax_thread(void *arg) {

    // Este hilo analiza el buffer en background y podría aplicar tags de color.

    // Utilizar <regex.h> para buscar patrones si la extensión es .c, .py, etc.

    while (E.running) {

        sleep(2);

        pthread_mutex_lock(&E.mutex_buffer);

        // ------------------ SECCIÓN DE IMPLEMENTACIÓN  ----------------
        //Leer la lista doblemente enlazada, aplicar regex a las palabras





        // clave configuradas en .nanorc y añadir caracteres ANSI de color al buffer visual.
        // ------------------ SECCIÓN DE IMPLEMENTACIÓN  ----------------



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


    snprintf(header, sizeof(header), "\x1b[7m %s - %d lineas | Col: %d, Fila: %d%s \x1b[K\x1b[m\r\n",

             E.filename, E.filas_totales, E.cx, E.cy, reloj);

    strcat(buffer, header);

}


void editor_draw_footer(const char *msg) {

    // Posicionar cursor en la última línea

    printf("\x1b[%d;1H", E.screenrows + 2);

    printf("\x1b[7m"); // Video inverso

    if (msg != NULL) {

        printf("%-80s", msg);

    } else {

        printf(" ^O Abrir | ^S Guardar | ^A G. Como | ^C Copiar | ^V Pegar | ^X Cortar | ^Q Salir ");

    }

    printf("\x1b[m\x1b[K"); // Restaurar formato y limpiar

}


void editor_refresh_screen() {

    char *buffer = malloc(8192);

    strcpy(buffer, "\x1b[?25l"); // Ocultar cursor
    strcat(buffer, "\x1b[H");    // Cursor al inicio


    editor_draw_header(buffer);


    // Dibujar área de texto (simplificado para el prototipo)

    pthread_mutex_lock(&E.mutex_buffer);

    Linea *l = E.primer_linea;

    for (int y = 0; y < E.screenrows; y++) {

        if (l != NULL) {

            strncat(buffer, l->texto, E.screencols);

            l = l->siguiente;

        } else {

            strcat(buffer, "~");

        }

        strcat(buffer, "\x1b[K\r\n"); // Limpiar línea

    }

    pthread_mutex_unlock(&E.mutex_buffer);


    printf("%s", buffer);

    editor_draw_footer(NULL);


    // Mover cursor a su posición real

    printf("\x1b[%d;%dH", (E.cy - E.rowoff) + 2, (E.cx - E.coloff) + 1);
    printf("\x1b[?25h"); // Mostrar cursor
    fflush(stdout);

    free(buffer);

}


// --- Procesamiento de Entrada ---

void editor_process_keypress() {

    char c = '\0';
    const char* fifo_path = "/tmp/CopyPaste_fifo"; //ruta al "buffer" de copia y pegue
    mkfifo(fifo_path, 0644); //creación del archivo pipe


    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) perror("read");


    if (c == '\0') return;


    switch (c) {

        case CTRL_KEY('q'): // Salir

            E.running = 0;
            printf("\x1b[2J\x1b[H"); // Limpiar pantalla
            exit(0);
        break;


        case CTRL_KEY('s'): // Guardar

            editor_draw_footer(" Guardando archivo... ");
            fflush(stdout);
            sleep(1); // Simulación
        break;


        case CTRL_KEY('c'): // Copiar

            // ------------------ SECCIÓN DE IMPLEMENTACIÓN  ----------------
            // TODO (Estudiantes): Implementar copiado al buffer de memoria
            

             int fd = open(fifo_path, O_RONLY);

            


            // ------------------ SECCIÓN DE IMPLEMENTACIÓN  ----------------
            editor_draw_footer("Contenido copiado ....");
        break;

           

        case CTRL_KEY('v'): // Pegar
            // ------------------ SECCIÓN DE IMPLEMENTACIÓN  ----------------
            // TODO (Estudiantes): Implementar inserción del buffer de memoria


            // ------------------ SECCIÓN DE IMPLEMENTACIÓN  ----------------

        break;

        // Movimiento básico (Flechas WASD o secuencias ANSI reales)

        case 'w': E.cy = (E.cy > 0) ? E.cy - 1 : 0; break;

        case 's': E.cy++; break;

        case 'a': E.cx = (E.cx > 0) ? E.cx - 1 : 0; break;

        case 'd': E.cx++; break;


        default:
            // ------------------ SECCIÓN DE IMPLEMENTACIÓN  ----------------
            // TODO (Estudiantes): Lógica de inserción de caracteres en la estructura de memoria




            // ------------------ SECCIÓN DE IMPLEMENTACIÓN  ----------------
        break;

    }

}


// --- MAIN ---

int main(int argc, char *argv[]) {

    enableRawMode();

    initEditor();


    if (argc >= 2) {

        strncpy(E.filename, argv[1], MAX_FILENAME - 1);

        // Simulación de carga inicial
        append_line("/* Bienvenido al editor (Fase 2) */", 35);
        append_line("int main() {", 12);
        append_line("    return 0;", 13);
        append_line("}", 1);

    } else {

        append_line("", 0);

    }


    // Iniciar Hilos
    pthread_t t_save, t_syntax;
    pthread_create(&t_save, NULL, autosave_thread, NULL);
    pthread_create(&t_syntax, NULL, syntax_thread, NULL);

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();

    }
    return 0;

}