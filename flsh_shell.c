#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_INPUT_SIZE 1024
#define MAX_ARGS 64
#define DELIMITADORES " \t\r\n\a" // Espacios, tabs, saltos de línea

// --- Función 1: Prompt Minimalista ---
void imprimir_prompt() {
    char cwd[1024];
    // Enfoque Minimalista: Solo mostramos el directorio actual y un símbolo ">"
    // Si hay error obteniendo el path, mostramos solo ">"
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("[%s]> ", cwd);
    } else {
        printf("> ");
    }
    fflush(stdout);
}

// --- Función 2: Parseo de línea (Tokenización) ---
// Transforma: "ls -l" -> ["ls", "-l", NULL]
// Retorna: Cantidad de argumentos encontrados
int parsear_comando(char *input, char **args) {
    int i = 0;
    char *token = strtok(input, DELIMITADORES); // 

    while (token != NULL && i < MAX_ARGS - 1) {
        args[i] = token;
        i++;
        token = strtok(NULL, DELIMITADORES);
    }
    args[i] = NULL; // La lista de argumentos debe terminar en NULL para execvp
    return i; // Retornamos la cantidad de argumentos
}

int main() {
    char input[MAX_INPUT_SIZE];
    char *args[MAX_ARGS]; // Array de punteros para los argumentos

    while (1) {
        imprimir_prompt();

        if (fgets(input, MAX_INPUT_SIZE, stdin) == NULL) {
            printf("\n");
            break; // Salida limpia con CTRL+D
        }

        // Paso de Parseo
        // Si el usuario solo dio Enter, args[0] será NULL
        if (parsear_comando(input, args) == 0) {
            continue;
        }

        // --- Detección de Comandos ---
        
        // 1. Comando Interno: exit
        if (strcmp(args[0], "exit") == 0) {
            // TODO: Agregar log obligatorio aquí [cite: 99]
            break;
        }
        
        // 2. Comando Interno: pwd (obligatorio [cite: 94])
        // Al ser minimalista, pwd es simplemente imprimir el getcwd
        else if (strcmp(args[0], "pwd") == 0) {
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                printf("%s\n", cwd);
            }
        }
        
        // 3. Comando Debug (Temporal para verificar el parser)
        else {
            printf("[DEBUG] Comando no reconocido aún: %s\n", args[0]);
            if (args[1] != NULL) {
                printf("[DEBUG] Argumento 1: %s\n", args[1]);
            }
        }
    }

    return 0;
}