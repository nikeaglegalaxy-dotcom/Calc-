# syslab

Учебная консольная утилита, демонстрирующая работу с системными вызовами POSIX.

## Сборка

```sh
make
# либо вручную:
cc -O2 -Wall -Wextra -std=c11 -o syslab syslab.c
```

## Использование

```
syslab <команда> [аргументы]
```

### 1. info — информация о файле (`stat`, `access`)

```sh
./syslab info file.txt
```

Пример вывода:

```
Path: file.txt
Type: regular file
Size: 1248 bytes
Readable: yes
Writable: yes
Executable: no
```

### 2. copy — копирование файла (`open`, `read`, `write`, `close`)

```sh
./syslab copy src.txt dst.txt
```

### 3. tail — вывод последних N строк (`open`, `read`, `lseek`, `write`, `close`)

```sh
./syslab tail file.txt 10
```

Файл читается с конца блоками по 4 КБ через `lseek`, что позволяет работать с большими файлами без загрузки их целиком в память.

### 4. run — запуск программы в дочернем процессе (`fork`, `execvp`, `waitpid`)

```sh
./syslab run ls -la /tmp
```

Возвращает код выхода дочернего процесса.
