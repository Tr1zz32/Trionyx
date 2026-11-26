# Sockets UNIX sencillos para Trionyx

API header-only para sockets locales POSIX `AF_UNIX` de tipo `SOCK_STREAM`. No agrega dependencias externas y evita `std::array`. Está en `trionyx::ipc`.

## Cliente compacto con `sockstream`

```cpp
#include "trionyx/unix_socket.hpp"
#include <string>

int main() {
    trionyx::ipc::sockstream socket;
    if (!socket.hear(std::string("/run/trionyx/service.sock"))) {
        return 1; // Revisar errno.
    }

    const std::string request = "hola";
    if (!socket.write(request)) {
        return 1;
    }

    auto response = socket.read();
    if (!response) {
        return 1; // Revisar errno.
    }
    // *response contiene el mensaje; puede ser vacío.
}
```

`hear(path)` conecta como cliente al socket del servidor. `write(string_view)` envía una trama binaria. `read()` devuelve `std::optional<std::string>`: usa `if (auto mensaje = socket.read())` y luego `*mensaje`; un resultado vacío (`""`) es un mensaje válido y `std::nullopt` indica error. El límite por defecto es 16 MiB; puedes pasar otro a `read(max_size)`. También sigue disponible `read(string, max_size)` para código existente. Aceptan rutas y payloads `std::string`; el contenido binario, incluido NUL, se conserva. Devuelven `false` al fallar y dejan el error en `errno`. Para un buffer fijo usa `read(buffer, capacity, received_size)`.

`sockstream` es un alias de `UnixStream` en `trionyx::ipc`, no una implementación distinta. No agrega nombres al espacio `trionyx::sysdata`, así que sysdata y sus consumidores de `UnixStream` permanecen compatibles. La API anterior (`connect`, `send_frame` y `receive_frame`) sigue disponible.

## Servidor

```cpp
#include "trionyx/unix_socket.hpp"
#include <string>
#include <unistd.h>

int main() {
    const std::string path = "/run/trionyx/service.sock";
    trionyx::ipc::UnixServer server;
    if (!server.listen(path)) {
        return 1; // errno contiene el error.
    }

    trionyx::ipc::UnixStream client = server.accept();
    if (!client.valid()) {
        server.close();
        ::unlink(path.c_str()); // UnixServer no elimina la ruta automáticamente.
        return 1;
    }

    std::string request;
    const bool ok = trionyx::ipc::receive_frame(client, request);
    server.close();
    ::unlink(path.c_str());
    return ok ? 0 : 1;
}
```

`UnixStream::connect` y `UnixServer::listen` admiten `const char*` y `std::string`. Rutas con bytes NUL incrustados se rechazan con `EINVAL`.

## Protocolo y límites

Cada trama contiene un entero sin signo de 32 bits con el tamaño del payload, en orden de red (big-endian), seguido por el payload binario. Las operaciones manejan lecturas/escrituras parciales e interrupciones `EINTR`. Los descriptores creados para conectar/escuchar y los aceptados llevan `FD_CLOEXEC`: se usa `SOCK_CLOEXEC` de forma atómica cuando está disponible (`accept4` en Linux), con `fcntl` como fallback.

Los métodos `sockstream::read()` y la sobrecarga `receive_frame(stream, string)` limitan el payload a 16 MiB por defecto (`default_max_frame_size`). Se puede cambiar con `receive_frame(stream, string, max_size)`. Si se excede el límite, devuelve `false` y fija `errno` a `EMSGSIZE`; el cuerpo no se consume, así que descarta/cierra esa conexión. El buffer de string crece dinámicamente y una falla de asignación puede propagar una excepción estándar. La sobrecarga de buffer fijo no realiza esa asignación.

Los errores de socket devuelven `false` y se consultan en `errno`; las operaciones de `UnixStream` y `UnixServer` no lanzan excepciones. `UnixStream` cierra su descriptor al destruirse. El servidor no desvincula automáticamente la ruta del socket.

## Pruebas y CI

Compilación y ejecución en Linux/POSIX, sin dependencias externas adicionales:

```sh
c++ -std=c++23 -Wall -Wextra -Werror -pedantic -pthread \
  -Iinclude tests/unix_socket_test.cpp -o /tmp/unix_socket_test
/tmp/unix_socket_test
```

GitHub Actions ejecuta el mismo conjunto de pruebas con GCC y Clang en cada push a `dev` o `main` y en pull requests hacia esas ramas. El workflow está en `.github/workflows/ci.yml`.
