#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <libgen.h>
#include <errno.h> // Necesario para identificar errores exactos

#define MAX_INPUT_SIZE 1024
#define MAX_ARGS 64
#define DELIMITADORES " \t\r\n\a"

// --- Prototipos para evitar warnings ---
void log_shell(char *cmd, char *detalles, int es_error);
// --- Función Auxiliar: Obtener Ruta de Logs ---
// Modificada para apuntar a /var/log/flsh
void obtener_ruta_logs(char *ruta_destino, size_t tamano) {
    // Definimos la ruta fija solicitada
    const char *ruta_fija = "/var/log/flsh";
    
    // Verificamos si tenemos acceso de escritura a esa ruta
    if (access(ruta_fija, W_OK) == 0) {
        // Si tenemos permiso, usamos la ruta del sistema
        snprintf(ruta_destino, tamano, "%s", ruta_fija);
    } else {
        // FALLBACK: Si no tenemos permisos en /var/log/flsh (no hiciste el sudo),
        // usamos una carpeta local para evitar que el programa falle.
        // Esto es una medida de seguridad "defensiva".
        fprintf(stderr, "[DEBUG] No hay permiso en %s. Usando ./logs local.\n", ruta_fija);
        
        // Obtenemos ruta local como plan B
        char ruta_exe[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", ruta_exe, sizeof(ruta_exe) - 1);
        if (len != -1) {
            ruta_exe[len] = '\0';
            char *dir = dirname(ruta_exe);
            snprintf(ruta_destino, tamano, "%s/logs", dir);
        } else {
            snprintf(ruta_destino, tamano, "./logs");
        }
    }
}

// --- Función de Logging (Ajustada) ---
void log_shell(char *cmd, char *detalles, int es_error) {
    char directorio_logs[PATH_MAX];
    char ruta_archivo[PATH_MAX];
    
    // 1. Obtener la ruta (/var/log/flsh o fallback local)
    obtener_ruta_logs(directorio_logs, sizeof(directorio_logs));
    
    // 2. Intentar asegurar que existe (por si usas el fallback local)
    // Nota: mkdir en /var/log fallará si no eres root, pero el access check de arriba lo maneja
    mkdir(directorio_logs, 0777); 

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char fecha[64];
    strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", tm);

    // 3. Definir nombre de archivo según PDF (shell.log o sistema_error.log)
    if (es_error) {
        snprintf(ruta_archivo, sizeof(ruta_archivo), "%s/sistema_error.log", directorio_logs);
    } else {
        snprintf(ruta_archivo, sizeof(ruta_archivo), "%s/shell.log", directorio_logs);
    }

    FILE *f = fopen(ruta_archivo, "a"); 
    if (f == NULL) {
        // Si falla aquí, es un error crítico de permisos que no pudimos evitar
        perror("[flsh_error]: Error fatal escribiendo log");
        return; 
    }

    char *usuario = getenv("USER");
    if (usuario == NULL) usuario = "unknown";

    fprintf(f, "[%s] User: %s | Cmd: %s | Detalles: %s\n", fecha, usuario, cmd, detalles);
    fclose(f);
}

// --- NUEVA FUNCIÓN DE SEGURIDAD (SANDBOX) ---
// Retorna 1 si es seguro proceder, 0 si hay violación de seguridad.
int validar_entorno_seguro(char *ruta_objetivo, const char *comando) {
    char *home = getenv("HOME");
    if (home == NULL) {
        log_shell((char*)comando, "SEGURIDAD: Variable HOME no definida", 1);
        fprintf(stderr, "[flsh_error]: Variable HOME no definida (ambiente inseguro)\n");
        return 0;
    }
 
    char ruta_resuelta[PATH_MAX];
    char *res = realpath(ruta_objetivo, ruta_resuelta);
 
    // Caso 1: La ruta NO existe aún (ej. mkdir nuevo_dir, cp a nuevo_archivo)
    if (res == NULL) {
        if (errno == ENOENT) {
            // Verificamos el directorio PADRE
            char *copia_ruta = strdup(ruta_objetivo);
            char *dir_padre = dirname(copia_ruta);
            char padre_resuelta[PATH_MAX];
 
            // Resolvemos la ruta del padre
            if (realpath(dir_padre, padre_resuelta) != NULL) {
                // Chequeamos si el padre está contenido en HOME
                if (strncmp(padre_resuelta, home, strlen(home)) == 0) {
                    free(copia_ruta);
                    return 1; // El padre es seguro, procedemos
                }
            }
            free(copia_ruta);
        }
        // Si falla realpath por otra razón o el padre no es seguro:
        char msg[512];
        snprintf(msg, sizeof(msg), "SEGURIDAD: Ruta invalida o fuera de limites: %s", ruta_objetivo);
        log_shell((char*)comando, msg, 1);
        fprintf(stderr, "[flsh_error]: Acceso denegado (SandBox). Ruta invalida o fuera de %s\n", home);
        return 0;
    }
 
    // Caso 2: La ruta YA existe
    // Verificamos que la ruta resuelta comience con la ruta del HOME
    if (strncmp(ruta_resuelta, home, strlen(home)) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "SEGURIDAD: Intento de escape de HOME hacia %s", ruta_resuelta);
        log_shell((char*)comando, msg, 1);
        fprintf(stderr, "[flsh_error]: Acceso denegado (SandBox). No puedes salir de %s\n", home);
        return 0;
    }
 
    return 1; // Es seguro
}
 
// --- Función 1: Prompt ---
void imprimir_prompt() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        // Indicamos visualmente que estamos en entorno seguro
        printf("[sandbox.flsh:%s]> ", cwd); 
    } else {
        printf("> ");
    }
    fflush(stdout);
}

// --- Función 2: Parseo ---
int parsear_comando(char *input, char **args) {
    int i = 0;
    char *token = strtok(input, DELIMITADORES);
    while (token != NULL && i < MAX_ARGS - 1) {
        args[i] = token;
        i++;
        token = strtok(NULL, DELIMITADORES);
    }
    args[i] = NULL;
    return i;
}

// --- Función 3: LS (Segurizada) ---
void ejecutar_ls(char *ruta) {
    // Si ruta es NULL es el directorio actual, que se asume seguro si ya estamos dentro.
    // Si se pasa argumento, hay que validarlo.
    if (ruta != NULL) {
        if (!validar_entorno_seguro(ruta, "ls")) return;
    }
 
    char *target = (ruta == NULL) ? "." : ruta;
    DIR *d = opendir(target);
    
    if (d == NULL) {
        perror("[flsh_error]: ls");
        log_shell("ls", "Fallo: Directorio no encontrado o sin permisos", 1);
        return;
    }
 
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] != '.') {
             printf("%s  ", dir->d_name);
        }
    }
    printf("\n");
    closedir(d);
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Exito: Listado directorio %s", target);
    log_shell("ls", msg, 0);
}
 
// --- Función 4: CD (Refactorizada con validar_entorno_seguro) ---
void ejecutar_cd(char *ruta) {
    char *home = getenv("HOME");
    if (home == NULL) return; // Ya se loguea error en validar si no existe home
 
    char *objetivo = (ruta == NULL) ? home : ruta;
 
    // Validación SandBox centralizada
    if (!validar_entorno_seguro(objetivo, "cd")) return;
 
    if (chdir(objetivo) != 0) {
        perror("[flsh_error]: cd");
        log_shell("cd", "Fallo: Error de sistema en chdir", 1);
    } else {
        char cwd[PATH_MAX];
        getcwd(cwd, sizeof(cwd));
        
        // Actualizar variable de entorno PWD
        static char env_var[PATH_MAX + 5]; 
        sprintf(env_var, "PWD=%s", cwd);
        putenv(env_var);
 
        char msg[512];
        snprintf(msg, sizeof(msg), "Exito: Cambio seguro a %s", cwd);
        log_shell("cd", msg, 0);
    }
}
 
// --- Función 5: MKDIR (Segurizada) ---
void ejecutar_mkdir(char *ruta) {
    if (ruta == NULL) {
        fprintf(stderr, "[flsh_error]: falta argumento mkdir\n");
        log_shell("mkdir", "Error: Falta argumento", 1);
        return;
    }
 
    // Validación SandBox
    if (!validar_entorno_seguro(ruta, "mkdir")) return;
 
    if (mkdir(ruta, 0755) != 0) {
        perror("[flsh_error]: mkdir");
        log_shell("mkdir", "Fallo: No se pudo crear directorio", 1);
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "Exito: Directorio %s creado", ruta);
        log_shell("mkdir", msg, 0);
    }
}
 
// --- Función 6: RM (Segurizada) ---
void ejecutar_rm(char *archivo) {
    if (archivo == NULL) {
        fprintf(stderr, "[flsh_error]: falta argumento rm\n");
        log_shell("rm", "Error: Falta argumento", 1);
        return;
    }
    
    // Validación SandBox
    if (!validar_entorno_seguro(archivo, "rm")) return;
 
    if (unlink(archivo) != 0) {
        perror("[flsh_error]: rm");
        log_shell("rm", "Fallo: No se pudo eliminar", 1);
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "Exito: Archivo %s eliminado", archivo);
        log_shell("rm", msg, 0);
    }
}
 
// --- Función 7: CP (Refactorizada con validar_entorno_seguro) ---
void ejecutar_cp(char *origen, char *destino) {
    if (origen == NULL || destino == NULL) {
        fprintf(stderr, "[flsh_error]: uso cp <origen> <destino>\n");
        log_shell("cp", "Error: Argumentos insuficientes", 1);
        return;
    }
 
    // Validación SandBox para AMBOS argumentos
    if (!validar_entorno_seguro(origen, "cp (origen)")) return;
    if (!validar_entorno_seguro(destino, "cp (destino)")) return;
 
    int fd_in = open(origen, O_RDONLY);
    if (fd_in < 0) {
        perror("[flsh_error]: cp (origen)");
        log_shell("cp", "Fallo: No se pudo abrir origen", 1);
        return;
    }
 
    int fd_out = open(destino, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) {
        perror("[flsh_error]: cp (destino)");
        close(fd_in);
        log_shell("cp", "Fallo: No se pudo crear destino", 1);
        return;
    }
 
    char buffer[1024];
    ssize_t bytes_leidos;
    int error_escritura = 0;
 
    while ((bytes_leidos = read(fd_in, buffer, sizeof(buffer))) > 0) {
        if (write(fd_out, buffer, bytes_leidos) != bytes_leidos) {
            perror("[flsh_error]: cp (error escritura)");
            error_escritura = 1;
            break;
        }
    }
 
    close(fd_in);
    close(fd_out);

    if (error_escritura) {
        log_shell("cp", "Fallo: Error durante la escritura", 1);
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "Exito: Copiado %s a %s", origen, destino);
        log_shell("cp", msg, 0);
    }
}
 
// --- Función 8: CAT (Segurizada) ---
void ejecutar_cat(char *archivo) {
    if (archivo == NULL) {
        fprintf(stderr, "[flsh_error]: falta argumento cat\n");
        log_shell("cat", "Error: Falta argumento", 1);
        return;
    }
 
    // Validación SandBox
    if (!validar_entorno_seguro(archivo, "cat")) return;
 
    int fd = open(archivo, O_RDONLY); 
    if (fd < 0) {
        perror("[flsh_error]: cat");
        log_shell("cat", "Fallo: Archivo no encontrado", 1);
        return;
    }
 
    char buffer[1024]; 
    ssize_t bytes_leidos;
    while ((bytes_leidos = read(fd, buffer, sizeof(buffer))) > 0) {
        write(STDOUT_FILENO, buffer, bytes_leidos);
    }
    printf("\n");
    close(fd);
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Exito: Leido %s", archivo);
    log_shell("cat", msg, 0);
}
 
// --- Función 9: ECHO ---
void ejecutar_echo(char **args) {
    int i = 1;
    while (args[i] != NULL) {
        printf("%s", args[i]);
        if (args[i + 1] != NULL) printf(" ");
        i++;
    }
    printf("\n");
    log_shell("echo", "Exito: Texto impreso", 0);
}
 
// --- Función 10: GREP (Segurizada) ---
void ejecutar_grep(char *patron, char *archivo) {
    if (patron == NULL || archivo == NULL) {
        fprintf(stderr, "[flsh_error]: uso grep <palabra> <archivo>\n");
        log_shell("grep", "Error: Argumentos insuficientes", 1);
        return;
    }
 
    // Validación SandBox
    if (!validar_entorno_seguro(archivo, "grep")) return;
 
    FILE *fp = fopen(archivo, "r");
    if (fp == NULL) {
        perror("[flsh_error]: grep");
        log_shell("grep", "Fallo: No se pudo abrir el archivo", 1);
        return;
    }
 
    char linea[1024];
    int coincidencias = 0;
    int num_linea = 0;
 
    while (fgets(linea, sizeof(linea), fp) != NULL) {
        num_linea++;
        if (strstr(linea, patron) != NULL) {
            printf("%d: %s", num_linea, linea);
            if (linea[strlen(linea) - 1] != '\n') {
                printf("\n");
            }
            coincidencias++;
        }
    }
 
    fclose(fp);
 
    char msg[256];
    if (coincidencias > 0) {
        snprintf(msg, sizeof(msg), "Exito: Encontradas %d coincidencias en %s", coincidencias, archivo);
    } else {
        snprintf(msg, sizeof(msg), "Info: Sin coincidencias para '%s' en %s", patron, archivo);
    }
    log_shell("grep", msg, 0);
}
 
// --- MAIN ---
int main() {
    char input[MAX_INPUT_SIZE];
    char *args[MAX_ARGS];
    char ruta_logs[PATH_MAX];
    obtener_ruta_logs(ruta_logs, sizeof(ruta_logs));
    
    // printf("--- Shell Iniciada (MODO SEGURO / SandBox) ---\n");
    // printf("Logs configurados en: %s\n", ruta_logs);
    
    while (1) {
        imprimir_prompt();
 
        if (fgets(input, MAX_INPUT_SIZE, stdin) == NULL) {
            printf("\n");
            break; 
        }
 
        size_t len = strlen(input);
        if (len > 0 && input[len-1] == '\n') {
            input[len-1] = '\0';
        }
 
        if (parsear_comando(input, args) == 0) {
            continue;
        }
 
        // --- MANEJO DE REDIRECCIÓN (Segurizado) ---
        int stdout_backup = -1;
        int redir_pos = -1;
        for (int k = 0; args[k] != NULL; k++) {
            if (strcmp(args[k], ">") == 0) {
                redir_pos = k;
                break;
            }
        }
 
        if (redir_pos != -1) {
            if (args[redir_pos + 1] == NULL) {
                fprintf(stderr, "[flsh_error]: error de sintaxis cerca de >\n");
                continue; 
            }
            char *archivo_destino = args[redir_pos + 1];
 
            // VALIDACION SandBox PARA REDIRECCION
            // Evita: echo "hack" > /etc/passwd
            if (!validar_entorno_seguro(archivo_destino, "redireccion >")) {
                // Si falla seguridad, abortamos el comando
                continue; 
            }
 
            stdout_backup = dup(STDOUT_FILENO);
            int fd_archivo = open(archivo_destino, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_archivo < 0) {
                perror("[flsh_error]: error redireccion");
                continue;
            }
            dup2(fd_archivo, STDOUT_FILENO);
            close(fd_archivo);
            args[redir_pos] = NULL; // Cortamos los argumentos
        }
 
        // --- EJECUCIÓN ---
 
        if (strcmp(args[0], "exit") == 0) {
            log_shell("exit", "Exito: Shell cerrada por usuario", 0);
            break;
        }
        else if (strcmp(args[0], "pwd") == 0) {
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd)) != NULL) {
                printf("%s\n", cwd);
                log_shell("pwd", "Exito", 0);
            }
        }
        else if (strcmp(args[0], "ls") == 0) ejecutar_ls(args[1]);
        else if (strcmp(args[0], "cd") == 0) ejecutar_cd(args[1]);
        else if (strcmp(args[0], "mkdir") == 0) ejecutar_mkdir(args[1]);
        else if (strcmp(args[0], "rm") == 0) ejecutar_rm(args[1]);
        else if (strcmp(args[0], "cp") == 0) ejecutar_cp(args[1], args[2]);
        else if (strcmp(args[0], "cat") == 0) ejecutar_cat(args[1]);
        else if (strcmp(args[0], "echo") == 0) ejecutar_echo(args);
        else if (strcmp(args[0], "grep") == 0) ejecutar_grep(args[1], args[2]);
        
        else {
            // Comandos Externos
            pid_t pid = fork();
            if (pid < 0) {
                perror("[flsh_error]: fork");
            } 
            else if (pid == 0) {
                execvp(args[0], args);
                perror("[flsh_error]: execvp");
                log_shell(args[0], "Fallo: Comando externo no encontrado", 1);
                exit(EXIT_FAILURE); 
            } 
            else {
                int status;
                wait(&status);
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                     log_shell(args[0], "Exito: Comando externo ejecutado", 0);
                } else {
                     log_shell(args[0], "Fallo: Error en comando externo", 1);
                }
            }
        }
 
        // Restaurar salida
        if (stdout_backup != -1) {
            fflush(stdout);
            dup2(stdout_backup, STDOUT_FILENO);
            close(stdout_backup);
        }
    }
 
    return 0;
}