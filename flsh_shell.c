#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h> // Necesario para opendir, readdir, closedir
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

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

// --- Función 3: Implementación de ls propio ---
void ejecutar_ls(char *ruta) {
    // Si no se especifica ruta, usamos el directorio actual "."
    if (ruta == NULL) {
        ruta = ".";
    }

    DIR *d = opendir(ruta); // Intenta abrir el directorio
    
    if (d == NULL) {
        // Enfoque minimalista: Error breve pero claro
        // perror imprime el error del sistema (ej: "No such file or directory")
        perror("mishell: ls"); 
        return;
    }

    struct dirent *dir;
    // Leemos entrada por entrada hasta que readdir devuelva NULL (fin del directorio)
    while ((dir = readdir(d)) != NULL) {
        // Opcional: Omitir archivos ocultos (los que empiezan con punto)
        if (dir->d_name[0] != '.') {
             printf("%s  ", dir->d_name);
        }
    }
    printf("\n"); // Salto de línea final para que el prompt no quede pegado

    closedir(d); // IMPORTANTE: Siempre cerrar lo que se abre
}

// --- Función 4: Implementación de cd (Corregida para Windows/Linux) ---
void ejecutar_cd(char *ruta) {
    // 1. Manejo de argumento vacío (ir a HOME)
    if (ruta == NULL) {
        ruta = getenv("HOME");
        if (ruta == NULL) {
            fprintf(stderr, "mishell: variable HOME no definida\n");
            return;
        }
    }

    // 2. Llamada al sistema chdir
    if (chdir(ruta) != 0) {
        perror("mishell: cd");
    } else {
        // 3. Actualización de variable de entorno
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            // ERROR ANTERIOR: setenv("PWD", cwd, 1);
            
            // SOLUCIÓN COMPATIBLE (Windows/Linux):
            // putenv requiere el formato "VARIABLE=valor"
            // Usamos una variable estática para que la memoria persista
            // (putenv a veces guarda el puntero, no una copia)
            static char env_var[1200]; 
            sprintf(env_var, "PWD=%s", cwd);
            putenv(env_var);
        }
    }
}

// --- Función 5: Implementación de mkdir ---
void ejecutar_mkdir(char *ruta) {
    // 1. Validación básica
    if (ruta == NULL) {
        fprintf(stderr, "mishell: falta el argumento para mkdir\n");
        return;
    }

    int resultado;

    // 2. Llamada al sistema (Diferenciada por SO)
    #ifdef _WIN32
        // En Windows, mkdir solo recibe el nombre
        resultado = mkdir(ruta);
    #else
        // En Linux, mkdir recibe nombre + permisos (0755 es el estándar: rwx r-x r-x)
        resultado = mkdir(ruta, 0755);
    #endif

    // 3. Manejo de errores
    if (resultado != 0) {
        // Esto cubrirá casos como: carpeta ya existe, ruta inválida, sin permisos
        perror("mishell: mkdir");
    }
    // Enfoque minimalista: Si tiene éxito, no imprimimos nada (silencio unix)
}

// --- Función 6: Implementación de rm ---
void ejecutar_rm(char *archivo) {
    // 1. Validación de argumentos
    if (archivo == NULL) {
        fprintf(stderr, "mishell: falta el argumento para rm\n");
        return;
    }

    // 2. Llamada al sistema unlink
    // unlink elimina la entrada del directorio. Si era el último enlace, libera el espacio.
    // NOTA: En Windows, unlink a veces se llama _unlink, pero muchos compiladores
    // modernos lo aceptan o puedes usar remove() de <stdio.h> que es estándar C.
    // Para este TP, usaremos unlink por ser la syscall POSIX pedida.
    
    if (unlink(archivo) != 0) {
        // Manejo de errores estándar (archivo no existe, sin permisos, es un directorio)
        perror("mishell: rm");
    }
    
    // Enfoque minimalista: Si funciona, silencio total.
}

// --- Función 7: Implementación de cp ---
void ejecutar_cp(char *origen, char *destino) {
    // 1. Validaciones
    if (origen == NULL || destino == NULL) {
        fprintf(stderr, "mishell: cp requiere dos argumentos (origen destino)\n");
        return;
    }

    // 2. Abrir archivo Origen
    // O_RDONLY: Read Only
    // O_BINARY: Importante para Windows (evita cambios de salto de linea), en Linux se ignora o define como 0
    #ifndef O_BINARY
    #define O_BINARY 0
    #endif

    int fd_in = open(origen, O_RDONLY | O_BINARY);
    if (fd_in < 0) {
        perror("mishell: cp (origen)");
        return;
    }

    // 3. Abrir/Crear archivo Destino
    // O_WRONLY: Write Only
    // O_CREAT: Crear si no existe
    // O_TRUNC: Si existe, borrar contenido anterior (sobrescribir)
    // 0644: Permisos rw-r--r-- (Lectura/Escritura dueño, Lectura otros)
    int fd_out = open(destino, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd_out < 0) {
        perror("mishell: cp (destino)");
        close(fd_in);
        return;
    }

    // 4. Bucle de Copia (Bufferizado)
    char buffer[1024]; // Leemos de a 1KB
    ssize_t bytes_leidos;

    // read devuelve la cantidad de bytes leidos. Si es 0, es Fin de Archivo (EOF).
    while ((bytes_leidos = read(fd_in, buffer, sizeof(buffer))) > 0) {
        // Escribimos exactamente la cantidad leida
        ssize_t bytes_escritos = write(fd_out, buffer, bytes_leidos);
        
        if (bytes_escritos != bytes_leidos) {
            perror("mishell: cp (error escritura)");
            break;
        }
    }

    // 5. Cerrar descriptores (Muy importante para liberar recursos)
    close(fd_in);
    close(fd_out);
}

// --- Función 8: Implementación de cat ---
void ejecutar_cat(char *archivo) {
    // 1. Validación
    if (archivo == NULL) {
        fprintf(stderr, "mishell: falta el argumento para cat\n");
        return;
    }

    // 2. Abrir el archivo
    // O_RDONLY: Solo lectura
    int fd = open(archivo, O_RDONLY | O_BINARY); 
    if (fd < 0) {
        perror("mishell: cat"); // Error si no existe o no hay permisos
        return;
    }

    // 3. Bucle de Lectura y Escritura a Pantalla
    char buffer[1024]; 
    ssize_t bytes_leidos;

    // Leemos del archivo...
    while ((bytes_leidos = read(fd, buffer, sizeof(buffer))) > 0) {
        // ... y escribimos directamente en el Descriptor 1 (STDOUT/Pantalla)
        // STDOUT_FILENO es una constante (usualmente 1) definida en unistd.h
        if (write(STDOUT_FILENO, buffer, bytes_leidos) != bytes_leidos) {
            perror("mishell: cat (error escribiendo en pantalla)");
            break;
        }
    }

    // 4. Cerrar el archivo (La pantalla NO se cierra)
    close(fd);
    
    // Opcional: Escribir un salto de línea al final por estética si el archivo no termina en \n
    // write(STDOUT_FILENO, "\n", 1); 
}

// --- Función 9: Implementación de echo ---
void ejecutar_echo(char **args) {
    // args[0] es "echo", así que empezamos desde args[1]
    int i = 1;
    while (args[i] != NULL) {
        printf("%s", args[i]);
        
        // Si hay otro argumento después, imprimimos un espacio para separarlos
        if (args[i + 1] != NULL) {
            printf(" ");
        }
        i++;
    }
    // Al final siempre imprimimos un salto de línea
    printf("\n");
}

// --- Función Principal: Bucle del Shell ---
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

        // Comando: ls
        if (strcmp(args[0], "ls") == 0) {
            // args[1] contiene la ruta (si el usuario la escribió) o NULL
            ejecutar_ls(args[1]);
        }
        
        // Comando: cd
        else if (strcmp(args[0], "cd") == 0) {
            // args[1] es la ruta destino (o NULL)
            ejecutar_cd(args[1]);
        }

        // Comando: mkdir
        else if (strcmp(args[0], "mkdir") == 0) {
            ejecutar_mkdir(args[1]);
        }

        // Comando: rm
        else if (strcmp(args[0], "rm") == 0) {
            ejecutar_rm(args[1]);
        }

        // Comando: cp
        else if (strcmp(args[0], "cp") == 0) {
            // args[1] es origen, args[2] es destino
            ejecutar_cp(args[1], args[2]);
        }

        // Comando: cat
        else if (strcmp(args[0], "cat") == 0) {
            ejecutar_cat(args[1]);
        }

        // Comando: echo
        else if (strcmp(args[0], "echo") == 0) {
            ejecutar_echo(args);
        }
        
        // --- Detección de Comandos ---
        
        // 1. Comando Interno: exit
        else if (strcmp(args[0], "exit") == 0) {
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