#ifndef MAP_H
#define MAP_H

#define MAX_MAPA_COLORES 48
#define MAX_PALABRA_MAP  32
#define MAX_CODIGO_MAP   16

typedef struct EntradaMapa {
    char palabra[MAX_PALABRA_MAP];
    char codigo[MAX_CODIGO_MAP];
    int activa;
} EntradaMapa;

typedef struct MapaColores {
    EntradaMapa entradas[MAX_MAPA_COLORES];
    int cantidad;
} MapaColores;

void mapa_inicializar(MapaColores *mapa);
int  mapa_agregar(MapaColores *mapa, const char *palabra, int codigo_ansi);
const char *mapa_buscar(const MapaColores *mapa, const char *token, int len);

#endif
