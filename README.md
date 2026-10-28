# Yarilo Display Server

**An experimental display server exploring a modular graphics stack for Linux, with FreeBSD support planned.**

Yarilo is an open-source project focused on developing a display-server architecture with explicit control over graphics resources, frame presentation, window management, and input events.

The project aims to build a modular alternative to the traditional display stack, with a core server, compositor, and external window manager.

## Current Implementation

The current prototype includes:

- **DRM/KMS output** — direct display control and hardware page flipping.
- **CPU-based composition** — rendering multiple client surfaces into a single frame.
- **External window manager** — window mapping, positioning, and stacking through IPC.
- **Client-server communication** — Unix-domain sockets using `SOCK_SEQPACKET`.
- **Shared-memory buffers** — validated client buffers with explicit lifetime tracking.
- **Frame presentation tracking** — reporting presented commits using hardware page-flip events.
- **Input subsystem** — Linux `libinput` integration through `libseat`.
- **Resource lifecycle management** — tracking buffer bindings, read leases, pending commits, and in-flight page flips.

The prototype has been built and tested with two clients on a physical 2560×1440 display at 60 Hz.

## Architecture

Yarilo is being developed around a few core principles:

- Clear separation between the core server, composition, and window-management responsibilities.
- Explicit resource ownership and buffer-lifetime tracking.
- Hardware-backed frame presentation through DRM/KMS.
- IPC-based communication between the server, clients, and external window manager.
- Safe handling of client disconnections, pending frames, and resource cleanup.

## Roadmap

Planned development includes:

- Completing input-event routing for pointer movement, mouse buttons, and keyboard events.
- Strengthening synchronization, error handling, and resource-lifecycle guarantees.
- Developing separate compatibility bridges for Wayland and X11.
- Adding a FreeBSD-specific platform and input backend.
- Expanding the test suite for multi-window operation, failure recovery, and long-running workloads.

These are development goals, not claims of already completed functionality.

## Development Status

**Experimental — active development.**

The current focus is on stabilizing the core server, multi-client composition, frame presentation, input routing, and safe teardown before expanding protocol compatibility.

---

# Yarilo Display Server

**Экспериментальный графический сервер, исследующий модульную графическую архитектуру для Linux. Поддержка FreeBSD запланирована.**

Yarilo — проект с открытым исходным кодом, посвящённый разработке собственной архитектуры графического сервера с явным управлением графическими ресурсами, выводом кадров, оконным менеджментом и событиями ввода.

Цель проекта — создать модульную альтернативу традиционному графическому стеку с ядром сервера, композитором и внешним оконным менеджером.

## Текущая реализация

Текущий прототип включает:

- **Вывод через DRM/KMS** — непосредственное управление дисплеем и аппаратное переключение кадровых буферов.
- **Композицию на CPU** — объединение нескольких поверхностей клиентов в единый кадр.
- **Внешний оконный менеджер** — управление отображением, расположением и порядком окон через IPC.
- **Межпроцессное взаимодействие** — Unix-сокеты с использованием `SOCK_SEQPACKET`.
- **Буферы в разделяемой памяти** — проверку клиентских буферов и отслеживание их жизненного цикла.
- **Отслеживание презентации кадров** — подтверждение отображённых коммитов по аппаратным событиям page flip.
- **Подсистему ввода** — интеграцию Linux `libinput` через `libseat`.
- **Управление жизненным циклом ресурсов** — учёт привязок буферов, аренд чтения, ожидающих коммитов и незавершённых page flip.

Прототип собран и протестирован на физическом дисплее с разрешением 2560×1440 и частотой 60 Гц при работе двух клиентов.

## Архитектура

Разработка Yarilo опирается на несколько основных принципов:

- Чёткое разделение ответственности между ядром сервера, композицией и оконным менеджером.
- Явное управление владением ресурсами и жизненным циклом буферов.
- Аппаратный вывод кадров через DRM/KMS.
- IPC-взаимодействие между сервером, клиентами и внешним оконным менеджером.
- Безопасная обработка отключения клиентов, незавершённых кадров и освобождения ресурсов.

## Дорожная карта

В дальнейшие планы входят:

- Завершение маршрутизации событий ввода: движения указателя, кнопок мыши и клавиатуры.
- Усиление синхронизации, обработки ошибок и гарантий жизненного цикла ресурсов.
- Разработка отдельных слоёв совместимости с Wayland и X11.
- Создание платформенного и входного бэкенда для FreeBSD.
- Расширение тестов многооконного режима, восстановления после сбоев и длительной работы.

Это цели дальнейшей разработки, а не утверждения о завершённой функциональности.

## Статус разработки

**Экспериментальный проект — активная разработка.**

Сейчас основной акцент сделан на стабилизации ядра сервера, композиции нескольких клиентских поверхностей, презентации кадров, маршрутизации ввода и безопасном завершении работы. Расширение совместимости с протоколами будет происходить по мере укрепления основной архитектуры.