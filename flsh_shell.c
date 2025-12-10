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
#include <errno.h> 

#define MAX_INPUT_SIZE 1024
#define MAX_ARGS 64
#define DELIMITADORES " \t\r\n\a"

// --- Prototipos ---
// Modificado: ahora acepta 'nivel' en lugar de 'es_error'
void log_shell(char *cmd, char *detalles, char *nivel);

// --- Función Auxiliar: Obtener Ruta de Logs ---
void obtener_ruta_logs(char *ruta_destino, size_t tamano) {
    const char *ruta_fija = "/var/log/flsh"; // Ajustado a tu carpeta correcta
    
    if (access(ruta_fija, W_OK) == 0) {
        snprintf(ruta_destino, tamano, "%s", ruta_fija);
    } else {
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

// --- NUEVA Función de Logging Detallado ---
void log_shell(char *cmd, char *detalles, char *nivel) {
    char directorio_logs[PATH_MAX];
    char ruta_archivo[PATH_MAX];
    obtener_ruta_logs(directorio_logs, sizeof(directorio_logs));
    
    mkdir(directorio_logs, 0755); 

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char fecha[64];
    strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", tm);

    // Lógica para separar archivos según criticidad
    if (strcmp(nivel, "ERROR") == 0 || strcmp(nivel, "CRITICAL") == 0) {
        snprintf(ruta_archivo, sizeof(ruta_archivo), "%s/sistema_error.log", directorio_logs);
    } else {
        snprintf(ruta_archivo, sizeof(ruta_archivo), "%s/shell.log", directorio_logs);
    }

    FILE *f = fopen(ruta_archivo, "a"); 
    if (f == NULL) return; 

    char *usuario = getenv("USER");
    if (usuario == NULL) usuario = "unknown";

    // --- OBTENCIÓN DE IP ---
    char ip_origen[64] = "LOCAL/CONSOLE";
    char *ssh_connection = getenv("SSH_CONNECTION"); 
    // SSH_CONNECTION fmt: "IP_CLIENTE PUERTO IP_SERVER PUERTO"
    if (ssh_connection != NULL) {
        char temp_ip[64];
        // Tomamos solo la primera palabra (la IP)
        if (sscanf(ssh_connection, "%63s", temp_ip) == 1) {
            strncpy(ip_origen, temp_ip, sizeof(ip_origen)-1);
        }
    }

    // Formato enriquecido: [FECHA] [NIVEL] IP | User | Cmd | Msg
    fprintf(f, "[%s] [%s] SRC:%s | USER:%s | CMD:%s | MSG:%s\n", 
            fecha, nivel, ip_origen, usuario, cmd, detalles);
    fclose(f);
}

// --- Helper para reportar errores ---
void reportar_error_sistema(char *cmd) {
    char *error_msg = strerror(errno);
    fprintf(stderr, "[flsh_error] %s: %s\n", cmd, error_msg);
    log_shell(cmd, error_msg, "ERROR"); // Usamos nivel ERROR
}

// --- Confirmación de acciones destructivas ---
int confirmar_accion(const char *mensaje) {
    printf("%s (s/n): ", mensaje);
    char respuesta[10];
    if (fgets(respuesta, sizeof(respuesta), stdin) != NULL) {
        if (respuesta[0] == 's' || respuesta[0] == 'S') return 1;
    }
    return 0;
}

// --- Lógica de Validación (SANDBOX) ---
int validar_ruta_en_home(char *ruta_input) {
    char *home = getenv("HOME");
    if (home == NULL) return 0;

    char ruta_resuelta[PATH_MAX];
    char *res = realpath(ruta_input, ruta_resuelta);

    if (res != NULL) {
        if (strncmp(ruta_resuelta, home, strlen(home)) == 0) return 1;
        return 0;
    }
    if (errno == ENOENT) {
        char *copia = strdup(ruta_input);
        char *padre = dirname(copia);
        char padre_resuelta[PATH_MAX];
        int es_seguro = 0;
        if (realpath(padre, padre_resuelta) != NULL) {
             if (strncmp(padre_resuelta, home, strlen(home)) == 0) es_seguro = 1;
        }
        free(copia);
        return es_seguro;
    }
    return 0;
}

int validar_entorno_seguro(char *ruta, const char *contexto) {
    if (validar_ruta_en_home(ruta)) {
        return 1;
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "Intento de acceso fuera de HOME: %s", ruta);
        log_shell((char*)contexto, msg, "WARNING"); // Nivel WARNING
        fprintf(stderr, "[flsh_sec]: Acceso denegado (SandBox).\n");
        return 0;
    }
}
 
void imprimir_prompt() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("[sandbox:%s]> ", cwd); 
    } else {
        printf("> ");
    }
    fflush(stdout);
}

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

// --- Comandos Internos ---

void ejecutar_ls(char *ruta) {
    if (ruta != NULL && !validar_entorno_seguro(ruta, "ls")) return;
    char *target = (ruta == NULL) ? "." : ruta;
    DIR *d = opendir(target);
    if (!d) { 
        reportar_error_sistema("ls");
        return; 
    }
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] != '.') printf("%s  ", dir->d_name);
    }
    printf("\n");
    closedir(d);
    log_shell("ls", "Listado exitoso", "INFO");
}
 
void ejecutar_cd(char *ruta) {
    char *home = getenv("HOME");
    char *objetivo = (ruta == NULL) ? home : ruta;
    if (!validar_entorno_seguro(objetivo, "cd")) return;
    
    if (chdir(objetivo) != 0) {
        reportar_error_sistema("cd");
    } else {
        char cwd[PATH_MAX];
        getcwd(cwd, sizeof(cwd));
        char env_var[PATH_MAX + 5]; 
        sprintf(env_var, "PWD=%s", cwd);
        putenv(env_var);
        log_shell("cd", cwd, "INFO");
    }
}
 
void ejecutar_mkdir(char *ruta) {
    if (!ruta) { fprintf(stderr, "mkdir: falta argumento\n"); return; }
    if (!validar_entorno_seguro(ruta, "mkdir")) return;
    
    if (mkdir(ruta, 0755) != 0) reportar_error_sistema("mkdir");
    else log_shell("mkdir", "Directorio creado", "INFO");
}
 
void ejecutar_rm(char *archivo) {
    if (!archivo) { fprintf(stderr, "rm: falta argumento\n"); return; }
    if (!validar_entorno_seguro(archivo, "rm")) return;
    
    // Confirmación
    char msg[512];
    snprintf(msg, sizeof(msg), "ALERTA: Vas a eliminar '%s'. ¿Estás seguro?", archivo);
    if (!confirmar_accion(msg)) {
        log_shell("rm", "Cancelado por usuario", "INFO");
        return;
    }

    if (unlink(archivo) != 0) reportar_error_sistema("rm");
    else log_shell("rm", "Archivo eliminado", "WARNING"); // Warning porque es destructivo
}
 
void ejecutar_cp(char *origen, char *destino) {
    if (!origen || !destino) { fprintf(stderr, "cp: faltan argumentos\n"); return; }
    if (!validar_entorno_seguro(origen, "cp in") || !validar_entorno_seguro(destino, "cp out")) return;
 
    int fd_in = open(origen, O_RDONLY);
    if (fd_in < 0) { reportar_error_sistema("cp (origen)"); return; }
    
    // Chequeo de existencia para confirmar sobrescritura
    struct stat st;
    if (stat(destino, &st) == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "ALERTA: '%s' ya existe. ¿Sobrescribir?", destino);
        if (!confirmar_accion(msg)) {
            close(fd_in);
            log_shell("cp", "Cancelado (sobrescritura)", "INFO");
            return;
        }
    }
 
    int fd_out = open(destino, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) { reportar_error_sistema("cp (destino)"); close(fd_in); return; }
 
    char buffer[1024];
    ssize_t n;
    while ((n = read(fd_in, buffer, sizeof(buffer))) > 0) write(fd_out, buffer, n);
 
    close(fd_in); close(fd_out);
    log_shell("cp", "Copia exitosa", "INFO");
}
 
void ejecutar_cat(char *archivo) {
    if (!archivo) { fprintf(stderr, "cat: falta argumento\n"); return; }
    if (!validar_entorno_seguro(archivo, "cat")) return;
    
    int fd = open(archivo, O_RDONLY);
    if (fd < 0) { reportar_error_sistema("cat"); return; }
    
    char buffer[1024];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) write(STDOUT_FILENO, buffer, n);
    printf("\n");
    close(fd);
    log_shell("cat", "Lectura exitosa", "INFO");
}

void ejecutar_grep(char *patron, char *archivo) {
    if (!patron || !archivo) { fprintf(stderr, "grep: faltan argumentos\n"); return; }
    if (!validar_entorno_seguro(archivo, "grep")) return;
    
    FILE *fp = fopen(archivo, "r");
    if (!fp) { reportar_error_sistema("grep"); return; }
    
    char linea[1024];
    int count = 0;
    while (fgets(linea, sizeof(linea), fp)) {
        if (strstr(linea, patron)) { printf("%s", linea); count++; }
    }
    fclose(fp);
    char msg[64]; snprintf(msg, 64, "Coincidencias: %d", count);
    log_shell("grep", msg, "INFO");
}

// --- MAIN ---
int main() {
    char input[MAX_INPUT_SIZE];
    char *args[MAX_ARGS];
    char *home = getenv("HOME");
    
    if (!home) { fprintf(stderr, "ERROR FATAL: HOME no definido.\n"); return 1; }
    
    while (1) {
        imprimir_prompt();
        if (!fgets(input, MAX_INPUT_SIZE, stdin)) break;
        
        size_t len = strlen(input);
        if (len > 0 && input[len-1] == '\n') input[len-1] = '\0';
        if (parsear_comando(input, args) == 0) continue;
 
        // --- Redirección ---
        int stdout_backup = -1, redir_pos = -1;
        for (int k = 0; args[k]; k++) if (strcmp(args[k], ">") == 0) redir_pos = k;
 
        if (redir_pos != -1) {
            if (!args[redir_pos + 1] || !validar_entorno_seguro(args[redir_pos + 1], ">")) continue;
            stdout_backup = dup(STDOUT_FILENO);
            int fd = open(args[redir_pos + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { reportar_error_sistema("redireccion"); continue; }
            dup2(fd, STDOUT_FILENO);
            close(fd);
            args[redir_pos] = NULL;
        }
 
        // --- Ejecución ---
        if (strcmp(args[0], "exit") == 0) { 
            log_shell("exit", "Sesion finalizada", "INFO"); 
            break; 
        }
        else if (strcmp(args[0], "pwd") == 0) {
             char cwd[1024]; getcwd(cwd, sizeof(cwd)); printf("%s\n", cwd);
             log_shell("pwd", "Exito", "INFO");
        }
        else if (strcmp(args[0], "ls") == 0) ejecutar_ls(args[1]);
        else if (strcmp(args[0], "cd") == 0) ejecutar_cd(args[1]);
        else if (strcmp(args[0], "mkdir") == 0) ejecutar_mkdir(args[1]);
        else if (strcmp(args[0], "rm") == 0) ejecutar_rm(args[1]);
        else if (strcmp(args[0], "cp") == 0) ejecutar_cp(args[1], args[2]);
        else if (strcmp(args[0], "cat") == 0) ejecutar_cat(args[1]);
        else if (strcmp(args[0], "echo") == 0) { 
            for(int i=1; args[i]; i++) printf("%s ", args[i]); printf("\n");
            log_shell("echo", "Exito", "INFO");
        }
        else if (strcmp(args[0], "grep") == 0) ejecutar_grep(args[1], args[2]);
        else {
            // --- Comandos Externos ---
            int violacion = 0;
            // Validaciones SandBox
            if (strchr(args[0], '/') != NULL && !validar_ruta_en_home(args[0])) violacion = 1;
            for (int k = 1; args[k] != NULL; k++) {
                if ((args[k][0] == '/' || (args[k][0] == '.' && args[k][1] == '.')) && !validar_ruta_en_home(args[k])) violacion = 1;
            }

            if (violacion) {
                fprintf(stderr, "[flsh_sec]: Ruta externa a HOME prohibida.\n");
                log_shell(args[0], "Intento escape sandbox", "CRITICAL");
            } else {
                pid_t pid = fork();
                if (pid == 0) {
                    execvp(args[0], args);
                    perror("[flsh_error]"); 
                    exit(127);
                } else {
                    int status;
                    wait(&status);
                    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                        log_shell(args[0], "Ejecucion externa OK", "INFO");
                    } else {
                        char msg[64];
                        snprintf(msg, sizeof(msg), "Fallo externo (Code: %d)", WEXITSTATUS(status));
                        log_shell(args[0], msg, "ERROR");
                    }
                }
            }
        }
 
        if (stdout_backup != -1) {
            fflush(stdout); dup2(stdout_backup, STDOUT_FILENO); close(stdout_backup);
        }
    }
    return 0;
}