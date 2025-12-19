# flsh
Shell enfocado en la velocidad de respuesta y seguridad del entorno, de pocas funcionalidades y con enfasis en la modalidad Sandbox para que los usuarios no puedan salir de su carpeta home.
## Gestión de Logs y Persistencia

El sistema de logging ha sido diseñado para ser resiliente a la falta de privilegios de administrador. [cite_start]Dado que el requisito de escribir en `/var/log` [cite: 144] generalmente requiere permisos de `root`, la shell implementa una detección automática de rutas:

* **Ruta Primaria:** Intenta escribir en `/var/log/flsh`.
* **Fallback Dinámico:** Si no hay acceso de escritura, la shell identifica su propia ubicación en el sistema de archivos (leyendo `/proc/self/exe`) y genera los logs en una carpeta `logs/` adyacente al ejecutable.

Esto permite que la shell sea "portable" y utilizada por usuarios estándar sin perder la capacidad de auditoría y registro de errores.

**Esta desición fue tomada en el caso de que dentro del LFS no pudieramos, o sea muy complicado el crear un usuario dentro del LFS que usara solo flsh**

## Sistema de Auditoría y Trazabilidad (Logging) implementado el 08/12

El Shell incorpora un motor de logging avanzado diseñado para entornos de seguridad y administración remota. A diferencia de un logger simple, este módulo implementa:

* **Segregación de Criticidad:** Cumpliendo con los estándares de diseño, los eventos se clasifican automáticamente:
    * `sistema_error.log`: Almacena exclusivamente errores de ejecución y fallos críticos (Niveles ERROR, CRITICAL).
    * `shell.log`: Registra la actividad estándar, cambios de directorio y ejecución de comandos exitosos.
* **Detección de Origen de Red:** El sistema es capaz de identificar si el comando fue ejecutado desde una consola local o una sesión SSH remota.
    * Analiza la variable de entorno `SSH_CONNECTION` para extraer y registrar la **IP de origen** del cliente.
    * Esto permite auditorías de seguridad detalladas ("¿Quién ejecutó `rm` y desde qué IP?").
* **Contexto Completo:** Cada entrada de log incluye Timestamp, Nivel, IP Origen, Usuario, Comando y Detalles del evento.

**El punto de hacer los loggins más detallados fue para darle ese enfoque de seguridad por sobre la shell ya construida**

## Gestión de Errores de Sistema (errno)  implementado el 08/12

El Shell implementa una rutina unificada para el reporte de fallos en llamadas al sistema (syscalls). En lugar de imprimir errores genéricos, el sistema:

1.  **Captura `errno`:** Lee el código de error específico devuelto por el Kernel tras una operación fallida (ej. `open`, `fork`, `exec`).
2.  **Decodificación:** Utiliza `strerror` para mostrar mensajes descriptivos (ej. *"Permission denied"*, *"No such file or directory"*).
3.  **Salida Estándar de Error (`stderr`):** Los errores se imprimen en el descriptor de archivo 2, garantizando que sean visibles incluso si el usuario está redirigiendo la salida del comando a un archivo.
4.  **Registro Automático:** Todo error reportado se archiva automáticamente en `sistema_error.log` para análisis posterior.

**Gestion de errores necesaria para la entrega del trabajo, con cuidado de cargar correctamente dentro de sistema_error.log**

## Prevención de Accidentes (Safety Interlocks)  implementado el 08/12

Siguiendo los principios de diseño de sistemas críticos, el Shell incorpora barreras de confirmación para comandos irreversibles:

* **Política de Confirmación:** Antes de ejecutar operaciones destructivas (como la eliminación de archivos con `rm` o la sobrescritura con `cp`), el sistema invoca una rutina de validación.
* **Enfoque "Fail-Safe":** La función de confirmación opera bajo el principio de "denegación por defecto". Si el usuario presiona Enter sin escribir 's', o introduce cualquier otro carácter, la operación se cancela automáticamente.
* **Seguridad en Entrada:** La lectura de la respuesta del usuario se realiza mediante `fgets` con límites estrictos de buffer, protegiendo al shell contra entradas malformadas o excesivamente largas.

**Para completar el enfoque de seguridad se tomó en cuenta la sugerencia de hacer una política de confirmación al manejar archivos, especificamente al moverlos o eliminarlos**

## Sandboxing de Sistema de Archivos implementado el 11/12

Para garantizar la integridad del sistema anfitrión y prevenir modificaciones accidentales o maliciosas fuera del espacio de usuario, el Shell implementa un **Sandbox estricto**:

* **Confinamiento en $HOME:** El shell restringe todas las operaciones de archivos (`cd`, `rm`, `cp`, `mkdir`) exclusivamente al directorio personal del usuario y sus subdirectorios.
* **Defensa contra Directory Traversal:** Utilizando `realpath`, el sistema resuelve todas las rutas antes de operarlas. Esto impide burlar la seguridad mediante rutas relativas complejas (ej. `cd ~/../../etc` es bloqueado exitosamente).
* **Validación Predictiva:** Para operaciones de escritura de nuevos archivos, el sistema valida la seguridad del directorio padre antes de permitir la creación, asegurando que nada se escriba fuera de los límites permitidos.

**Esta funcionalidad fue pensada como un sello distintivo de la shell flsh, siendo el primer paso para darle el enfoque de seguridad a la shell**

## Protocolo de Respuesta ante Incidentes

El Shell no solo restringe el acceso, sino que implementa un protocolo activo de **Detección y Respuesta**:

* **Auditoría de Intentos (Warning Level):** Cualquier intento de acceder a archivos fuera del área permitida (`$HOME`) es interceptado y registrado en el log con nivel `WARNING`. Esto permite diferenciar entre errores de sistema (bugs) e intentos de violación de políticas de seguridad.
* **Feedback Inmediato:** El usuario recibe una notificación explícita en consola con la etiqueta `[flsh_sec]`, indicando que la operación no falló por error técnico, sino que fue bloqueada intencionalmente por el Sandbox.
* **Transparencia:** El log registra el comando exacto (contexto) y la ruta que detonó la alerta, facilitando la revisión posterior por parte del administrador.

**Funcionalidad mejorada para los registros de auditoría, para enfatizar el enfoque de sandbox del proyecto**

## Interfaz de Usuario (Prompt Dinámico) implementado el 28/11

El Shell implementa una interfaz de línea de comandos (CLI) contextual:

* **Prompt Informativo:** A diferencia de un prompt estático, el shell informa constantemente al usuario sobre su ubicación actual en el sistema de archivos (Current Working Directory).
* **Gestión de I/O:** Se implementa un manejo explícito del flujo de salida estándar (`stdout`). Debido a la naturaleza bufferizada de la terminal en sistemas UNIX, se fuerza el vaciado del buffer (`fflush`) tras imprimir el prompt. Esto asegura que la invitación a escribir aparezca siempre antes de que el sistema se bloquee esperando la entrada del usuario, evitando condiciones de carrera visuales.

**Prompt de solicitud de entrada, obligatorio**

## Comandos
### Comando Interno: ls implementado el 28/11

Se ha desarrollado una implementación propia del comando de listado, prescindiendo de llamadas al sistema externo (`system("ls")`).

- **Técnica:** Manipulación directa de estructuras de directorio mediante `<dirent.h>` (`opendir`, `readdir`).
- **Seguridad:** Integración total con el módulo de *Sandboxing* para impedir la lectura de directorios restringidos fuera del espacio del usuario.
- **Funcionalidad:** Soporta listado del directorio actual (por defecto) o rutas absolutas/relativas, aplicando filtros para ocultar archivos de sistema (dotfiles).


### Comando Interno: cd implementado el 28/11
Módulo encargado de la navegación por el sistema de archivos.
- **Implementación Nativa:** Utiliza la syscall `chdir` en lugar de invocar subprocesos, permitiendo que el cambio de directorio persista en el Shell.
- **Consistencia de Entorno:** Cumpliendo con los requisitos del TP, cada cambio de directorio actualiza dinámicamente la variable de entorno `PWD` (`putenv`). Esto garantiza que los procesos lanzados posteriormente hereden la ruta de trabajo correcta.
- **Seguridad:** Incorpora validación previa de rutas para evitar salir del área designada por el Sandbox.


### Comando Interno: mkdir implementado el 28/11
Implementación segura para la creación de directorios.
- **Lógica Directa:** Utiliza la syscall `mkdir` definiendo explícitamente los permisos de acceso (`0755` - lectura y ejecución pública, escritura privada).
- **Protección:** Validada por el *Sandbox*, impide la creación de carpetas fuera de la jerarquía del usuario, previniendo la contaminación de directorios del sistema.
- **Robustez:** Maneja errores comunes como directorio ya existente (`EEXIST`) o ruta inválida.


### Comando Interno: cp implementado el 28/11
Implementación de bajo nivel para la duplicación de archivos.
- **Mecanismo I/O:** Prescinde de funciones de alto nivel, implementando un bucle de transferencia directa mediante syscalls `read`/`write` y un buffer intermedio. Esto permite copiar cualquier tipo de archivo (texto o binario).
- **Seguridad de Datos:** Incorpora lógica de detección de conflictos. Antes de escribir, verifica la existencia del destino (`stat`); si el archivo existe, el Shell pausa la ejecución y solicita autorización para sobrescribir.
- **Sandboxing Dual:** Valida tanto la ruta de lectura como la de escritura, asegurando que la operación de copia se mantenga estrictamente dentro de los límites del usuario.


### Comando Interno: cat implementado el 29/11
Visualizador de contenido implementado mediante I/O directa.
- **Eficiencia de Memoria:** Utiliza un buffer de tamaño fijo para leer y mostrar el archivo por fragmentos ("chunks"). Esto permite visualizar archivos muy grandes sin necesidad de cargarlos completamente en la memoria RAM.
- **Salida Directa:** Emplea la syscall `write` sobre el descriptor `STDOUT_FILENO`, lo que permite una salida rápida y sin el overhead de formateo de librerías estándar.
- **Privacidad:** Integrado con el sistema de seguridad, impide que un usuario utilice el shell para leer archivos de configuración del sistema operativo fuera de su directorio personal.


### Comando Opcional: grep (Análisis de Texto) implementado el 30/11
Funcionalidad extendida para la búsqueda de cadenas dentro de archivos (Feature opcional +2 ptos).
- **Motor de Búsqueda Propio:** Implementación nativa de búsqueda lineal utilizando la función `strstr` de la librería estándar de C, sin dependencias externas.
- **Manejo de Streams:** Utiliza lectura bufferizada orientada a líneas (`fgets`) para procesar archivos de texto de manera eficiente.
- **Integración de Seguridad:** Mantiene la coherencia con el resto del shell aplicando las mismas restricciones de *Sandbox* para evitar la lectura de logs del sistema o archivos protegidos fuera del `HOME`.
- **Logging Enriquecido:** El sistema registra en la bitácora no solo la ejecución del comando, sino la cantidad exacta de coincidencias encontradas ("hits").

### Comando Interno: echo implementado el 28/11
Utilidad fundamental para la visualización de texto y prueba de descriptores de salida.
- **Implementación Inline:** Integrado directamente en el bucle principal (`main`) para máxima velocidad de respuesta.
- **Iteración de Argumentos:** Recorre y concatena los argumentos recibidos separándolos por espacios, finalizando con un salto de línea estándar.
- **Soporte de Redirección:** Gracias a la arquitectura del Shell, `echo` puede utilizarse para crear o escribir archivos de texto simple (ej. `echo hola mundo > saludo.txt`). Al manipular los *file descriptors* antes de la ejecución del comando, la salida de `printf` es capturada transparentemente por el archivo destino.

### Comando Interno: rm (Remove) implementado el 29/11
Gestor de eliminación segura de archivos.
- **Implementación:** Utiliza la syscall `unlink()` para eliminar la referencia del inodo.
- **Interlock de Seguridad:** Antes de proceder, invoca la función `confirmar_accion()`. Si el usuario no escribe explícitamente 's', la operación se aborta.
- **Auditoría:** Registra en `shell.log` con nivel WARNING si el archivo fue borrado, o INFO si el usuario canceló la operación.

### Comando Interno: pwd (Print Working Directory) implementado el 28/11
- **Función:** Muestra la ruta absoluta actual.
- **Lógica:** Aunque el prompt ya muestra la ruta, este comando permite obtenerla limpia para scripts o verificaciones. Utiliza `getcwd()` y la imprime en salida estándar.

### Comando Interno: exit implementado el 28/11
- **Función:** Cierre controlado de la sesión.
- **Requisito de Trazabilidad:** Antes de terminar el proceso con `exit(0)`, escribe una entrada final en el log ("Sesión finalizada"), permitiendo calcular la duración de la sesión del usuario en auditorías posteriores.

## Motor de Redirección de I/O implementado el 29/11

El Shell soporta la redirección de salida estándar (`>`), permitiendo volcar el resultado de cualquier comando a un archivo en lugar de la pantalla.

**Implementación Técnica:**
A diferencia de shells que parsean toda la línea, nuestra implementación manipula la tabla de descriptores de archivo (File Descriptors) **antes** de la ejecución del comando:

1.  **Parsing:** Se detecta el token `>`. El argumento siguiente se trata como el archivo destino.
2.  **Backup:** Se duplica el descriptor original de la terminal (`STDOUT_FILENO`) usando `dup()`, para poder restaurarlo después.
3.  **Apertura:** Se abre el archivo destino con flags `O_WRONLY | O_CREAT | O_TRUNC`.
4.  **Sustitución (dup2):** Se utiliza `dup2(fd_archivo, STDOUT_FILENO)`.
    * Esto hace que, para el sistema operativo, el descriptor 1 (salida estándar) apunte ahora al archivo.
    * El comando ejecutado (sea `echo`, `ls` o un externo) escribe en "pantalla" sin saber que en realidad está escribiendo en el disco.
5.  **Restauración:** Al finalizar, se utiliza `dup2` con el backup para devolver el control a la terminal del usuario.

## Arquitectura de Ejecución de Procesos (Externos) implementado el 30/11

Para los comandos que no son internos (como `vim`, `nano`, `top` o scripts de usuario), el Shell implementa el ciclo de vida estándar de procesos UNIX, gestionando manualmente la memoria y el control de flujo.

**Flujo de Implementación:**

1.  **Detección:** Si el comando ingresado no coincide con ningún *built-in* (ls, cd, etc.), se trata como un binario externo.
2.  **Validación de Seguridad:** Antes de ejecutar, el sistema verifica que los argumentos (rutas de archivos) no violen el *Sandbox* del usuario.
3.  [cite_start]**Clonación (Forking):** Se invoca la syscall `fork()`[cite: 140].
    * Esto crea un proceso hijo idéntico al padre.
    * El shell (padre) retiene el control para no cerrarse.
4.  **Superposición (Exec):** El proceso hijo llama a `execvp()`.
    * Se busca el binario en las rutas definidas en la variable `$PATH`.
    * Si tiene éxito, la imagen de memoria del hijo es reemplazada por el nuevo programa.
    * Si falla (ej. comando no existe), se imprime el error y se fuerza la salida con `exit(127)`.
5.  **Sincronización (Wait):** El proceso padre utiliza `wait(&status)` para bloquearse hasta que el hijo termine. Esto evita la creación de procesos "zombies" y permite registrar en el log si el programa externo terminó exitosamente o con error.


### Participantes y contribuciones:
* Main REPL: Igor Dedoff
* Comandos internos: Ivan Paredes
* Comandos externos y aplicación de las funciones de seguridad: Federico Recalde
