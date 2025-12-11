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
void log_shell(char *cmd, char *detalles, char *nivel);

/* * Determina la ruta absoluta donde se almacenarán los archivos de log ('shell.log' y 'sistema_error.log').
 * La función implementa una estrategia de prioridades para garantizar la persistencia:
 * 1. Intenta usar el directorio del sistema '/var/log/shell'.
 * 2. Verifica permisos de escritura con access(W_OK); si falla (ej. usuario sin privilegios root),
 * activa un mecanismo de 'fallback'.
 * 3. Resuelve la ubicación real del binario en ejecución mediante el enlace simbólico 
 * '/proc/self/exe' (específico de Linux) para establecer una carpeta '/logs' relativa al ejecutable.
 * Esto asegura que la shell pueda registrar eventos sin importar desde dónde se invoque o los permisos del usuario.
 */
void obtener_ruta_logs(char *ruta_destino, size_t tamano) {
    const char *ruta_fija = "/var/log/shell";
    
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


/*
 * Orquesta el sistema de auditoría y registro de eventos del Shell.
 * Funcionalidad:
 * 1. Resolución de Entorno: Obtiene la ruta dinámica de logs y asegura la existencia del
 * directorio destino mediante mkdir (0755), evitando fallos por carpetas inexistentes.
 * 2. Clasificación de Criticidad: Implementa lógica de bifurcación para separar flujos:
 * - Niveles 'ERROR'/'CRITICAL' -> se escriben en 'sistema_error.log'.
 * - Niveles informativos -> se escriben en 'shell.log'.
 * 3. Enriquecimiento de Datos (Contexto de Seguridad):
 * - Identifica al usuario del sistema (getenv USER).
 * - Detecta el origen de la conexión analizando 'SSH_CONNECTION'. Si la sesión es remota,
 * extrae la IP del cliente; si es local, marca como 'LOCAL/CONSOLE'.
 * 4. Persistencia: Escribe una entrada estructurada y temporalizada (timestamp) en modo 'append'.
 */
void log_shell(char *cmd, char *detalles, char *nivel) {
    char directorio_logs[PATH_MAX];
    char ruta_archivo[PATH_MAX];
    obtener_ruta_logs(directorio_logs, sizeof(directorio_logs));
    
    // Intentar crear el directorio por si no existe (mkdir es idempotente si ya existe con -p, 
    // pero en C estándar validamos o ignoramos error EEXIST).
    mkdir(directorio_logs, 0755); 

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char fecha[64];
    strftime(fecha, sizeof(fecha), "%Y-%m-%d %H:%M:%S", tm);

    // Lógica para separar archivos según criticidad (Requisito TP)
    if (strcmp(nivel, "ERROR") == 0 || strcmp(nivel, "CRITICAL") == 0) {
        snprintf(ruta_archivo, sizeof(ruta_archivo), "%s/sistema_error.log", directorio_logs);
    } else {
        snprintf(ruta_archivo, sizeof(ruta_archivo), "%s/shell.log", directorio_logs);
    }

    FILE *f = fopen(ruta_archivo, "a"); 
    if (f == NULL) return; 

    char *usuario = getenv("USER");
    if (usuario == NULL) usuario = "unknown";

    // --- OBTENCIÓN DE IP (Valor Agregado: Seguridad/Red) ---
    char ip_origen[64] = "LOCAL/CONSOLE";
    char *ssh_connection = getenv("SSH_CONNECTION"); 
    // SSH_CONNECTION fmt: "IP_CLIENTE PUERTO IP_SERVER PUERTO"
    if (ssh_connection != NULL) {
        char temp_ip[64];
        // Tomamos solo la primera palabra (la IP del cliente)
        if (sscanf(ssh_connection, "%63s", temp_ip) == 1) {
            strncpy(ip_origen, temp_ip, sizeof(ip_origen)-1);
        }
    }

    // Formato enriquecido: [FECHA] [NIVEL] IP | User | Cmd | Msg
    fprintf(f, "[%s] [%s] SRC:%s | USER:%s | CMD:%s | MSG:%s\n", 
            fecha, nivel, ip_origen, usuario, cmd, detalles);
    fclose(f);
}


/*
 * Centraliza la gestión de errores del sistema (System Call Errors).
 * Funcionalidad:
 * 1. Traducción: Recupera la variable global 'errno' (establecida por la última llamada al sistema fallida)
 * y la convierte en una descripción textual legible mediante 'strerror'.
 * 2. Visualización: Imprime el error en el flujo estándar de error (stderr), separándolo del flujo 
 * de salida normal (stdout) para mantener la limpieza en redirecciones y pipes.
 * 3. Persistencia: Invoca a 'log_shell' con nivel "ERROR" para asegurar que el incidente quede 
 * registrado en 'sistema_error.log', cumpliendo con los requisitos de auditoría.
 */
void reportar_error_sistema(char *cmd) {
    char *error_msg = strerror(errno);
    // fprintf a stderr asegura que el usuario vea el error incluso si redirige stdout
    fprintf(stderr, "[flsh_error] %s: %s\n", cmd, error_msg);
    log_shell(cmd, error_msg, "ERROR"); // Usamos nivel ERROR
}


/*
 * Implementa un mecanismo de seguridad interactivo (Fail-Safe) para validar operaciones críticas.
 * Funcionalidad:
 * 1. Interrupción de Flujo: Detiene la ejecución automática para solicitar consentimiento explícito 
 * del usuario a través de la salida estándar (stdout).
 * 2. Lectura Segura de Buffer: Utiliza 'fgets' en lugar de 'scanf' o 'gets' para leer de 'stdin'. 
 * Esto previene vulnerabilidades de desbordamiento de búfer (buffer overflow) y maneja correctamente 
 * los caracteres de nueva línea.
 * 3. Lógica de Decisión: Evalúa el primer carácter de la entrada. Retorna 1 (verdadero) solo si 
 * la intención es afirmativa ('s' o 'S'); cualquier otra entrada resulta en un retorno 0 (falso), 
 * abortando la operación destructiva por defecto (deny-by-default).
 */
int confirmar_accion(const char *mensaje) {
    printf("%s (s/n): ", mensaje);
    char respuesta[10];
    // fgets es seguro porque limitamos la lectura a sizeof(respuesta)
    if (fgets(respuesta, sizeof(respuesta), stdin) != NULL) {
        if (respuesta[0] == 's' || respuesta[0] == 'S') return 1;
    }
    return 0;
}

/*
 * Implementa un mecanismo de contención de sistema de archivos (Filesystem Sandboxing).
 * Funcionalidad:
 * 1. Resolución Canónica: Utiliza 'realpath' para resolver enlaces simbólicos y referencias relativas 
 * (como '..' o '.'), convirtiendo la entrada en una ruta absoluta pura. Esto mitiga ataques de 
 * "Directory Traversal" (ej. intentar acceder a /etc/passwd desde home usando ../../../).
 * 2. Verificación de Prefijo: Compara la ruta resuelta con la variable de entorno 'HOME'. Si la ruta 
 * no comienza con el prefijo de HOME, se deniega el acceso.
 * 3. Manejo de Archivos Inexistentes (Look-ahead): Si el archivo destino no existe (errno == ENOENT), 
 * el sistema extrae y valida el directorio padre. Esto es crucial para comandos de creación (mkdir, cp) 
 * donde el destino final aún no está en el disco, pero debemos asegurar que se creará en una ubicación permitida.
 */
int validar_ruta_en_home(char *ruta_input) {
    char *home = getenv("HOME");
    if (home == NULL) return 0; // Sin HOME definido, bloqueamos por seguridad (Fail-closed)

    char ruta_resuelta[PATH_MAX];
    char *res = realpath(ruta_input, ruta_resuelta);

    // Caso 1: El archivo/directorio ya existe
    if (res != NULL) {
        // Verificamos si ruta_resuelta empieza con la ruta de home
        if (strncmp(ruta_resuelta, home, strlen(home)) == 0) return 1;
        return 0;
    }
    
    // Caso 2: El archivo no existe (ej. creando nuevo dir), validamos el padre
    if (errno == ENOENT) {
        char *copia = strdup(ruta_input); // Duplicamos porque dirname puede modificar el string
        char *padre = dirname(copia);
        char padre_resuelta[PATH_MAX];
        int es_seguro = 0;
        
        // Resolvemos la ruta absoluta del directorio padre
        if (realpath(padre, padre_resuelta) != NULL) {
             // Validamos que el padre esté dentro del HOME
             if (strncmp(padre_resuelta, home, strlen(home)) == 0) es_seguro = 1;
        }
        free(copia); // Liberamos memoria dinámica
        return es_seguro;
    }
    
    return 0;
}

// --- Wrapper de Seguridad: Validación y Reporte ---

/*
 * Actúa como "Middleware" de seguridad, encapsulando la lógica de validación del Sandbox
 * con el sistema de reporte y auditoría.
 * Funcionalidad:
 * 1. Verificación: Invoca a 'validar_ruta_en_home' para determinar si la operación es lícita.
 * 2. Auditoría de Seguridad: Si la validación falla, genera automáticamente una entrada de log
 * con nivel "WARNING", documentando el intento de violación del perímetro de seguridad.
 * 3. Feedback al Usuario: Informa inmediatamente a la salida estándar de error (stderr) que
 * la acción fue bloqueada por el módulo [flsh_sec], garantizando transparencia.
 * Retorno: 1 si es seguro proceder, 0 si la acción fue bloqueada.
 */
int validar_entorno_seguro(char *ruta, const char *contexto) {
    // Delegamos la verificación matemática/lógica a la función de Sandbox
    if (validar_ruta_en_home(ruta)) {
        return 1; // Acceso concedido
    } else {
        // --- Bloque de Gestión de Incidentes ---
        char msg[256];
        // Construimos el mensaje forense con la ruta infractora
        snprintf(msg, sizeof(msg), "Intento de acceso fuera de HOME: %s", ruta);
        
        // Registramos el incidente en los logs persistentes (Nivel WARNING, no ERROR de sistema)
        log_shell((char*)contexto, msg, "WARNING"); 
        
        // Notificamos al usuario final sobre el bloqueo
        fprintf(stderr, "[flsh_sec]: Acceso denegado (SandBox).\n");
        
        return 0; // Acceso denegado
    }
}
 
/*
 * Renderiza el indicador de línea de comandos (Prompt) para la interacción usuario-sistema.
 * Funcionalidad:
 * 1. Contexto Espacial: Utiliza la llamada al sistema 'getcwd' (Get Current Working Directory)
 * para obtener la ruta absoluta actual y mostrarla al usuario. Esto es vital para la orientación
 * dentro del árbol de directorios.
 * 2. Manejo de Buffers (Crucial): Dado que 'printf' suele usar "line-buffering" (espera un '\n'
 * para imprimir en pantalla) y el prompt NO termina en nueva línea (para que el usuario escriba
 * al lado), es obligatorio llamar a 'fflush(stdout)'. Esto fuerza el vaciado del buffer y garantiza
 * que el prompt aparezca inmediatamente antes de la espera de entrada (input).
 * 3. Robustez: Si 'getcwd' falla (ej. permisos o ruta eliminada), muestra un prompt genérico "> "
 * para mantener la operatividad.
 */
void imprimir_prompt() {
    char cwd[1024];
    // Intentamos recuperar el directorio de trabajo actual
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        // Personalización del prompt (Requisito de "Tema/Enfoque propio")
        printf("[Úsame: %s]> ", cwd); 
    } else {
        // Fallback en caso de error de sistema al leer la ruta
        printf("> ");
    }
    // Forzamos la salida a pantalla porque printf no tiene '\n' al final
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

// --- Comando Built-in: ls (Listar Directorio) ---

/*
 * Implementación nativa de listado de archivos mediante API POSIX (dirent.h), sin dependencias externas.
 * Funcionalidad:
 * 1. Validación de Seguridad: Si se especifica una ruta, verifica que esté dentro del Sandbox permitido
 * antes de intentar abrirla.
 * 2. Acceso al Sistema de Archivos: Utiliza 'opendir' y 'readdir' para obtener el flujo de entradas
 * del directorio, manejando punteros de estructura 'DIR' y 'struct dirent'.
 * 3. Filtrado Visual: Implementa lógica para omitir archivos ocultos (que inician con '.'), 
 * emulando el comportamiento estándar de una shell.
 * 4. Gestión de Errores: Captura fallos de apertura (ej. permisos, ruta inexistente) y los reporta.
 */
void ejecutar_ls(char *ruta) {
    // Si hay argumento, verificamos que sea seguro (Sandbox)
    if (ruta != NULL && !validar_entorno_seguro(ruta, "ls")) return;

    // Si ruta es NULL, listamos el directorio actual (".")
    char *target = (ruta == NULL) ? "." : ruta;

    DIR *d = opendir(target);
    if (!d) { 
        reportar_error_sistema("ls");
        return; 
    }

    struct dirent *dir;
    // Iteramos sobre cada entrada del directorio
    while ((dir = readdir(d)) != NULL) {
        // Filtramos archivos ocultos (los que empiezan con punto)
        if (dir->d_name[0] != '.') printf("%s  ", dir->d_name);
    }
    printf("\n");
    
    closedir(d); // Es vital cerrar el stream del directorio
    log_shell("ls", "Listado exitoso", "INFO");
}
 
// --- Comando Built-in: cd (Change Directory) ---

/*
 * Gestiona el cambio del directorio de trabajo del proceso actual (Shell).
 * Funcionalidad:
 * 1. Resolución de Destino: Si no se provee argumento, redirige al usuario a su 'HOME' (comportamiento estándar).
 * 2. Validación de Seguridad: Intercepta la operación con el Sandbox ('validar_entorno_seguro') antes de llamar al kernel.
 * 3. Cambio de Contexto: Ejecuta la llamada al sistema 'chdir' para modificar el estado del proceso.
 * 4. Actualización de Entorno (Requisito Crítico): Tras un cambio exitoso, actualiza la variable de entorno 'PWD'
 * mediante 'putenv'. Esto asegura que las futuras llamadas a 'getcwd' y los procesos hijos hereden la ruta correcta.
 */
void ejecutar_cd(char *ruta) {
    char *home = getenv("HOME");
    char *objetivo = (ruta == NULL) ? home : ruta;
    
    // Verificamos permisos antes de intentar movernos
    if (!validar_entorno_seguro(objetivo, "cd")) return;
    
    // Intentamos cambiar el directorio
    if (chdir(objetivo) != 0) {
        reportar_error_sistema("cd");
    } else {
        // Actualizamos la variable PWD para mantener consistencia
        char cwd[PATH_MAX];
        getcwd(cwd, sizeof(cwd));
        char env_var[PATH_MAX + 5]; 
        sprintf(env_var, "PWD=%s", cwd);
        putenv(env_var); // Modifica el entorno del proceso actual
        log_shell("cd", cwd, "INFO");
    }
}
 
// --- Comando Built-in: mkdir (Make Directory) ---

/*
 * Crea un nuevo directorio en el sistema de archivos.
 * Funcionalidad:
 * 1. Validación de Argumentos: Verifica la existencia del nombre del directorio antes de proceder.
 * 2. Seguridad (Sandbox): Confirma mediante 'validar_entorno_seguro' que el nuevo directorio
 * se creará dentro de los límites permitidos ($HOME), evitando escrituras en zonas de sistema.
 * 3. Llamada al Sistema: Invoca 'mkdir' con modo octal 0755 (rwxr-xr-x), otorgando permisos completos
 * al usuario y de lectura/ejecución al grupo y otros.
 * 4. Gestión de Resultados: Reporta errores de sistema (ej. "File exists") o registra el éxito en el log.
 */
void ejecutar_mkdir(char *ruta) {
    if (!ruta) { fprintf(stderr, "mkdir: falta argumento\n"); return; }
    
    // Verificamos que sea seguro crear aquí
    if (!validar_entorno_seguro(ruta, "mkdir")) return;
    
    // 0755 = rwx (Dueño) | r-x (Grupo) | r-x (Otros)
    if (mkdir(ruta, 0755) != 0) reportar_error_sistema("mkdir");
    else log_shell("mkdir", "Directorio creado", "INFO");
}
 
// --- Comando Built-in: rm (Remove File) ---

/*
 * Elimina un archivo del sistema de archivos de forma segura y auditada.
 * Funcionalidad:
 * 1. Validación de Seguridad (Sandbox): Verifica mediante 'validar_entorno_seguro' que el archivo
 * objetivo se encuentre dentro del espacio de usuario permitido, previniendo el borrado de archivos del sistema.
 * 2. Confirmación Interactiva (Fail-Safe): Implementa una barrera de seguridad lógica ('confirmar_accion')
 * que detiene la ejecución hasta obtener consentimiento explícito del usuario. Esto mitiga el error humano.
 * 3. Ejecución Atómica: Utiliza la llamada al sistema 'unlink' (estándar POSIX) para eliminar la referencia
 * del archivo en el inodo correspondiente.
 * 4. Auditoría Crítica: Registra el evento con nivel "WARNING" (si fue exitoso) o "INFO" (si fue cancelado),
 * permitiendo trazar quién borró qué y cuándo.
 */
void ejecutar_rm(char *archivo) {
    if (!archivo) { fprintf(stderr, "rm: falta argumento\n"); return; }
    
    // Capa 1: Validación de Entorno (Sandbox)
    if (!validar_entorno_seguro(archivo, "rm")) return;
    
    // Capa 2: Confirmación de Usuario (Requisito de Seguridad)
    char msg[512];
    snprintf(msg, sizeof(msg), "ALERTA: Vas a eliminar '%s'. ¿Estás seguro?", archivo);
    
    if (!confirmar_accion(msg)) {
        // Registro de la cancelación (Auditoría positiva)
        log_shell("rm", "Cancelado por usuario", "INFO");
        return;
    }

    // Capa 3: Ejecución (Syscall unlink)
    if (unlink(archivo) != 0) reportar_error_sistema("rm");
    else log_shell("rm", "Archivo eliminado", "WARNING"); // Warning porque es destructivo
}
 
// --- Comando Built-in: cp (Copy File) ---

/*
 * Realiza la copia de archivos binarios o de texto mediante gestión de buffers de bajo nivel.
 * Funcionalidad:
 * 1. Validación Dual: Verifica que TANTO el origen COMO el destino estén dentro del entorno seguro
 * (Sandbox), previniendo exfiltración de datos o escritura en zonas prohibidas.
 * 2. Protección contra Sobrescritura: Utiliza 'stat' para detectar si el destino ya existe. 
 * Si es así, detiene el flujo y solicita confirmación explicita al usuario, cumpliendo con la 
 * política de seguridad para operaciones destructivas.
 * 3. Gestión de Archivos (Low-Level I/O):
 * - Origen: Se abre en modo Solo Lectura (O_RDONLY).
 * - Destino: Se abre con flags O_CREAT (crear si no existe) y O_TRUNC (vaciar si existe),
 * con permisos 0644 (rw-r--r--).
 * 4. Transferencia Bufferizada: Implementa un bucle de lectura/escritura usando un buffer de 1KB.
 * Esto asegura eficiencia en memoria, copiando chunks de datos en lugar de cargar todo el archivo en RAM.
 */
void ejecutar_cp(char *origen, char *destino) {
    if (!origen || !destino) { fprintf(stderr, "cp: faltan argumentos\n"); return; }
    
    // Verificamos seguridad en ambos extremos: no leer de /etc, no escribir en /bin
    if (!validar_entorno_seguro(origen, "cp in") || !validar_entorno_seguro(destino, "cp out")) return;

    int fd_in = open(origen, O_RDONLY);
    if (fd_in < 0) { reportar_error_sistema("cp (origen)"); return; }
    
    // --- Bloque de Prevención de Accidentes ---
    struct stat st;
    // Si stat devuelve 0, el archivo destino existe
    if (stat(destino, &st) == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "ALERTA: '%s' ya existe. ¿Sobrescribir?", destino);
        // Solicitamos confirmación interactiva antes de truncar el archivo
        if (!confirmar_accion(msg)) {
            close(fd_in);
            log_shell("cp", "Cancelado (sobrescritura)", "INFO");
            return;
        }
    }

    // Abrimos destino: O_WRONLY (escribir), O_CREAT (crear), O_TRUNC (borrar contenido previo)
    // Permisos 0644: Usuario(rw), Grupo(r), Otros(r)
    int fd_out = open(destino, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) { reportar_error_sistema("cp (destino)"); close(fd_in); return; }

    // --- Bucle de Copia (Core) ---
    char buffer[1024];
    ssize_t n;
    // Leemos del origen al buffer y escribimos del buffer al destino
    while ((n = read(fd_in, buffer, sizeof(buffer))) > 0) write(fd_out, buffer, n);

    close(fd_in); close(fd_out);
    log_shell("cp", "Copia exitosa", "INFO");
}
 
// --- Comando Built-in: cat (Concatenate/Display) ---

/*
 * Visualiza el contenido de un archivo volcándolo directamente a la salida estándar.
 * Funcionalidad:
 * 1. Validación de Seguridad: Confirma mediante 'validar_entorno_seguro' que el archivo objetivo
 * reside dentro del perímetro permitido ($HOME), impidiendo la lectura no autorizada de archivos 
 * del sistema (como /etc/passwd).
 * 2. Acceso de Bajo Nivel: Abre el archivo utilizando la syscall 'open' en modo solo lectura (O_RDONLY).
 * 3. Transferencia Bufferizada: Implementa un ciclo de lectura/escritura ('read' -> 'write') usando 
 * un buffer de 1KB. Escribe directamente en STDOUT_FILENO (descriptor 1), lo cual es más eficiente 
 * y seguro para datos binarios que usar funciones de alto nivel como 'printf'.
 * 4. Gestión de Recursos: Garantiza el cierre del descriptor de archivo ('close') al finalizar, 
 * evitando fugas de recursos en el shell.
 */
void ejecutar_cat(char *archivo) {
    if (!archivo) { fprintf(stderr, "cat: falta argumento\n"); return; }
    
    // Verificamos permisos de lectura según políticas del Sandbox
    if (!validar_entorno_seguro(archivo, "cat")) return;
    
    int fd = open(archivo, O_RDONLY);
    if (fd < 0) { reportar_error_sistema("cat"); return; }
    
    char buffer[1024];
    ssize_t n;
    // Leemos del archivo y escribimos directamente a la salida estándar (pantalla)
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) write(STDOUT_FILENO, buffer, n);
    
    printf("\n"); // Salto de línea estético al final
    close(fd);
    log_shell("cat", "Lectura exitosa", "INFO");
}

// --- Comando Built-in Opcional: grep (Global Regular Expression Print) ---

/*
 * Implementa una utilidad de búsqueda de patrones de texto simple dentro de archivos.
 * Funcionalidad:
 * 1. Objetivo (Valor Agregado): Cumple con el requerimiento opcional del TP de procesar texto 
 * y buscar cadenas específicas sin invocar utilitarios externos.
 * 2. Seguridad (Sandbox): Al igual que los comandos críticos, valida mediante 'validar_entorno_seguro' 
 * que el archivo a analizar resida en el espacio de usuario permitido ($HOME).
 * 3. Procesamiento de Texto: Utiliza I/O estándar (stdio.h) con 'fopen' y 'fgets'. Esto es ideal 
 * para lectura línea por línea, a diferencia de la lectura por bloques de bytes usada en 'cp'.
 * 4. Algoritmo de Búsqueda: Emplea 'strstr' (string.h) para verificar la existencia de la subcadena 
 * 'patron' dentro de cada línea leída.
 * 5. Auditoría Estadística: No solo registra el éxito de la operación, sino que contabiliza y loguea 
 * el número exacto de coincidencias encontradas, enriqueciendo la información de auditoría.
 */
void ejecutar_grep(char *patron, char *archivo) {
    if (!patron || !archivo) { fprintf(stderr, "grep: faltan argumentos\n"); return; }
    
    // Verificamos permisos de lectura (Sandbox)
    if (!validar_entorno_seguro(archivo, "grep")) return;
    
    FILE *fp = fopen(archivo, "r");
    if (!fp) { reportar_error_sistema("grep"); return; }
    
    char linea[1024];
    int count = 0;
    // Leemos el archivo línea por línea hasta el final (EOF)
    while (fgets(linea, sizeof(linea), fp)) {
        // strstr devuelve un puntero si encuentra el patrón, o NULL si no
        if (strstr(linea, patron)) { 
            printf("%s", linea); // Imprimimos la línea completa que contiene el patrón
            count++; 
        }
    }
    fclose(fp);
    
    // Registro detallado con métricas
    char msg[64]; snprintf(msg, 64, "Coincidencias: %d", count);
    log_shell("grep", msg, "INFO");
}

// --- Comando Built-in: shutdown (Apagar Sistema) ---

/*
 * Inicia la secuencia de apagado del sistema operativo.
 * Funcionalidad:
 * 1. Confirmación Crítica: Utiliza 'confirmar_accion' para evitar apagados accidentales.
 * 2. Auditoría: Registra el evento con nivel "CRITICAL".
 * 3. Persistencia de Datos: Ejecuta 'sync()' para asegurar que los buffers de disco se guarden.
 * 4. Ejecución Privilegiada: Intenta invocar el comando del sistema usando 'sudo' primero,
 * ya que un usuario estándar no tiene permisos para apagar el hardware.
 */
void ejecutar_shutdown() {
    // 1. Capa de Seguridad Interactiva
    if (!confirmar_accion("PELIGRO: Esto apagará el equipo completo. ¿Estás seguro?")) {
        log_shell("shutdown", "Cancelado por usuario", "INFO");
        return;
    }

    log_shell("shutdown", "Iniciando secuencia de apagado...", "CRITICAL");
    
    // 2. Sincronización de discos (Buena práctica antes de apagar)
    sync(); 

    // 3. Ejecución del comando de sistema
    // Hacemos fork para manejar el error si el comando 'shutdown' no existe o falla.
    pid_t pid = fork();
    if (pid == 0) {
        // Proceso Hijo
        
        // Opción A: Intentar con sudo (lo más probable para usuario normal)
        char *args_sudo[] = {"sudo", "shutdown", "-h", "now", NULL};
        execvp("sudo", args_sudo);
        
        // Opción B: Si sudo falla (o no existe), intentar directo (por si ya somos root)
        // Si execvp anterior funcionó, esta línea nunca se ejecuta.
        char *args_direct[] = {"shutdown", "-h", "now", NULL};
        execvp("shutdown", args_direct);

        // Si llegamos aquí, ambos fallaron
        reportar_error_sistema("shutdown (fallo al invocar comando de sistema)");
        exit(1);
    } else {
        // Proceso Padre: Esperamos a ver si el comando se ejecutó
        int status;
        wait(&status);
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr, "[flsh_error] shutdown: No se pudo apagar (¿Faltan permisos sudo?).\n");
        }
    }
}

// --- MAIN: Bucle Principal de Ejecución (REPL) ---
/*
 * Punto de entrada y orquestador del Shell. Implementa el ciclo de vida "Read-Eval-Print Loop".
 * Arquitectura y Flujo:
 * 1. Inicialización: Valida variables de entorno críticas ($HOME) para garantizar la integridad del Sandbox.
 * 2. Captura de Entrada: Utiliza 'fgets' para leer la línea de comandos de manera segura.
 * 3. Procesamiento de Redirección (I/O Redirection):
 * - Detecta el operador '>'.
 * - Valida la seguridad del archivo destino (Sandbox).
 * - Manipula la tabla de descriptores de archivo (File Descriptors) usando 'dup' (backup de stdout) 
 * y 'dup2' (reemplazo de stdout por el archivo). Esto permite que la salida de CUALQUIER comando 
 * (printf o write) se escriba en el archivo de forma transparente.
 * 4. Despacho de Comandos (Dispatcher):
 * - Enrutamiento estático para comandos internos (built-ins): ls, cd, rm, echo, etc.
 * 5. Ejecución de Comandos Externos (Process Creation):
 * - Si no es interno, verifica violaciones de seguridad en los argumentos.
 * - Implementa el patrón estándar UNIX:
 * a. fork(): Clona el proceso actual.
 * b. Child: Llama a 'execvp' para reemplazar su imagen de memoria con el nuevo programa.
 * c. Parent: Usa 'wait' para bloquearse hasta que el hijo termine, recogiendo su estado de salida (exit code).
 * 6. Restauración: Al final del ciclo, recupera el stdout original para volver a mostrar el prompt en pantalla.
 */
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
        // Asumimos que parsear_comando divide input en tokens y devuelve cont
        if (parsear_comando(input, args) == 0) continue; 

        // --- Redirección ---
        int stdout_backup = -1, redir_pos = -1;
        for (int k = 0; args[k]; k++) if (strcmp(args[k], ">") == 0) redir_pos = k;

        if (redir_pos != -1) {
            // Validamos que el archivo destino sea seguro antes de abrirlo
            if (!args[redir_pos + 1] || !validar_entorno_seguro(args[redir_pos + 1], ">")) continue;
            
            stdout_backup = dup(STDOUT_FILENO); // Guardamos la terminal original
            
            // 0644 = rw-r--r--
            int fd = open(args[redir_pos + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { reportar_error_sistema("redireccion"); continue; }
            
            dup2(fd, STDOUT_FILENO); // Reemplazamos stdout con el archivo
            close(fd);
            args[redir_pos] = NULL; // Cortamos los argumentos para que el comando no vea el '>'
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
        else if (strcmp(args[0], "shutdown") == 0) ejecutar_shutdown();
        else if (strcmp(args[0], "ls") == 0) ejecutar_ls(args[1]);
        else if (strcmp(args[0], "cd") == 0) ejecutar_cd(args[1]);
        else if (strcmp(args[0], "mkdir") == 0) ejecutar_mkdir(args[1]);
        else if (strcmp(args[0], "rm") == 0) ejecutar_rm(args[1]);
        else if (strcmp(args[0], "cp") == 0) ejecutar_cp(args[1], args[2]);
        else if (strcmp(args[0], "cat") == 0) ejecutar_cat(args[1]);
        else if (strcmp(args[0], "echo") == 0) { 
            // Implementación inline de echo
            for(int i=1; args[i]; i++) printf("%s ", args[i]); printf("\n");
            log_shell("echo", "Exito", "INFO");
        }
        else if (strcmp(args[0], "grep") == 0) ejecutar_grep(args[1], args[2]);
        else {
            // --- Comandos Externos ---
            int violacion = 0;
            // Validaciones SandBox para comandos externos (ej. /bin/ls o ../script.sh)
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
                    // Proceso Hijo
                    execvp(args[0], args);
                    // Si execvp retorna, hubo error (ej. comando no encontrado)
                    perror("[flsh_error]"); 
                    exit(127);
                } else {
                    // Proceso Padre
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

        // Restaurar stdout si hubo redirección
        if (stdout_backup != -1) {
            fflush(stdout); dup2(stdout_backup, STDOUT_FILENO); close(stdout_backup);
        }
    }
    return 0;
}