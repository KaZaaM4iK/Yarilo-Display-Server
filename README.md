# Yarilo Display Server

**Один бинарник. Один процесс. Полная совместимость с Wayland и X11 одновременно. Без Xwayland. Навсегда.**

Yarilo — это радикально новый дисплейный стек, который похоронит и X11, и Wayland, оставив от них только то, что реально нужно в 2025+ году.

### Принципы, от которых мы никогда не отступим:
- Один единственный бинарник — `aurora-compositor`
- 100 % нативная совместимость с Wayland (KDE, GNOME, Sway, Firefox, Steam — без перекомпиляции)
- 100 % нативная совместимость с X11 (xeyes, glxgears, wine, софт 90-х — без Xwayland навсегда)
- Работает из коробки на Linux + FreeBSD + OpenBSD + NetBSD + DragonFlyBSD с первого стабильного релиза
- Собственный минимальный X11-сервер в panic-restartable треде (не форк Xorg, наш с нуля)
- Новый протокол Yarilo Protocol v1 — настоящий «Wayland 2.0»
- Рендеринг: Vulkan/wgpu основа, Zink для legacy OpenGL
- Всё на современном Rust 2025-style (1.82+, async, wgpu 0.21+, safety-first)

Цель: **Октябрь 2026 — Yarilo 1.0 в Arch, Fedora, FreeBSD ports, OpenBSD ports.**

Мы не делаем очередной Wayland-клон.  
Мы делаем историю.
