# Trionyx

Trionyx es una distribución GNU/Linux x86_64 en desarrollo. Este repositorio también contiene componentes de sistema y utilidades de bajo nivel en C/C++.

Tambien, se caracteriza el `system-init`, sistema de inicializacion (init) propio de Trionyx, algunas de sus caracteristicas son:

## API sencilla de sockets UNIX

La API header-only está en `include/trionyx/unix_socket.hpp`, dentro del espacio de nombres `trionyx::ipc`. Usa sockets locales POSIX `AF_UNIX` de tipo `SOCK_STREAM`, sin dependencias externas ni `std::array`; ofrece `sockstream.hear(path)`, `read` y `write` y mantiene compatibles `UnixStream` y las funciones libres. Los descriptores se crean con `SOCK_CLOEXEC` cuando está disponible.

### Uso rápido con `sockstream` y `std::string`

```cpp
#include "trionyx/unix_socket.hpp"
#include <string>

int main() {
    trionyx::ipc::sockstream socket;
    if (!socket.hear(std::string("/run/trionyx/service.sock"))) return 1;
    if (!socket.write(std::string("hola"))) return 1;
    auto response = socket.read();
    if (!response) return 1; // revisar errno
}
```

`read()` devuelve `std::optional<std::string>`: `*response` es el mensaje; vacío también puede ser un frame válido y `std::nullopt` significa error. `sockstream` es un alias de `UnixStream` en `trionyx::ipc`; no altera el espacio de nombres `trionyx::sysdata`. Referencia completa en [`Documentation/UNIX_SOCKETS_SPANISH.md`](Documentation/UNIX_SOCKETS_SPANISH.md).

El protocolo de trama tiene un tamaño de 32 bits en orden de red, seguido por los bytes del payload. La referencia completa está en [`Documentation/UNIX_SOCKETS_SPANISH.md`](Documentation/UNIX_SOCKETS_SPANISH.md).

El servicio `sysdata` de init añade `GET`/`SET` y `GETP`/`SETP` por `/dev/socket/sysdata`, con claves validadas (sin espacios, ni `.` al inicio/final), dos stores globales (normal y privada), identidad de proceso validada por PID y tiempo de inicio, y una proyección best-effort de consulta en `/dev/reg` que nunca condiciona la tabla autoritativa. Consulta [`Documentation/SYSDATA_SPANISH.md`](Documentation/SYSDATA_SPANISH.md); el punto de entrada de init debe invocar `run_init_service()` para arrancarlo en un pthread detached; ambos stores están protegidos por mutexes pthread.

El helper de salida `stdr::out` de `include/types.h` también acepta rutas y contenido `std::string`; las rutas con NUL incrustado se rechazan con `EINVAL`.

### Compilar y ejecutar las pruebas

En Linux con un compilador C++23 y pthreads:

```sh
c++ -std=c++23 -Wall -Wextra -Werror -pedantic -pthread \
  -Iinclude tests/unix_socket_test.cpp -o /tmp/trionyx-unix-socket-test
/tmp/trionyx-unix-socket-test
```

Las pruebas cubren `sockstream`, tramas binarias, NUL dentro de `std::string`, el límite de tamaño, una comunicación cliente-servidor, la salida a archivo de `stdr::out` con una ruta `std::string` y el protocolo `sysdata` (permisos, claves y limpieza por PID).

```sh
c++ -std=c++23 -Wall -Wextra -Werror -pedantic -pthread \
  -Iinclude tests/sysdata_test.cpp -o /tmp/trionyx-sysdata-test
/tmp/trionyx-sysdata-test
```

```sh
c++ -std=c++23 -Wall -Wextra -Werror -pedantic \
  -Iinclude tests/types_out_test.cpp src/builtin.cpp \
  -o /tmp/trionyx-types-out-test
/tmp/trionyx-types-out-test
```

### CI

GitHub Actions compila y ejecuta las pruebas en cada push a `dev` o `main` y en pull requests hacia esas ramas, usando GCC y Clang. El workflow está en `.github/workflows/ci.yml`.

## Estado del proyecto

La API de sockets UNIX es una utilidad pequeña e independiente dentro del repositorio. No implica que la distribución o todos sus componentes estén completos o validados como sistema arrancable.
