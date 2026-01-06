use anyhow::Result;
use std::panic;
use std::sync::Arc;
use tokio::{signal, sync::broadcast};
use tracing::{error, info, warn};

pub struct AuroraSupervisor {
    shutdown_tx: broadcast::Sender<()>,
}

impl AuroraSupervisor {
    pub fn new() -> Self {
        let (shutdown_tx, _) = broadcast::channel(1);
        Self { shutdown_tx }
    }

    /// Спавнит задачу с автоматическим рестартом при панике
    async fn spawn_restartable<Fut>(
        &self,
        name: &'static str,
        mut task_factory: impl FnMut() -> Fut + Send + 'static,
    ) where
        Fut: std::future::Future<Output = Result<()>> + Send + 'static,
    {
        let mut shutdown_rx = self.shutdown_tx.subscribe();
        tokio::spawn(async move {
            loop {
                let task = panic::catch_unwind(panic::AssertUnwindSafe(|| task_factory()));
                let result = match task {
                    Ok(fut) => fut.await,
                    Err(_) => {
                        error!("{name} ПАНИКА — перезапуск через 1с");
                        Err(anyhow::anyhow!("panic"))
                    }
                };

                if result.is_err() {
                    warn!("{name} упал, рестарт через 1 секунду");
                    tokio::select! {
                        _ = tokio::time::sleep(tokio::time::Duration::from_secs(1)) => {},
                        _ = shutdown_rx.recv() => {
                            info!("{name} shutdown во время рестарта");
                            return;
                        }
                    }
                    continue;
                }

                // Нормальный выход — ждём shutdown
                tokio::select! {
                    _ = shutdown_rx.recv() => {
                        info!("{name} получил shutdown");
                        return;
                    }
                }
            }
        });
    }

    pub async fn run(self) -> Result<()> {
        info!("AURORA SUPERVISOR STARTED — один процесс, вечная жизнь");

        // Заглушка для Wayland
        self.spawn_restartable("Wayland Server", || async {
            info!("Wayland task стартовал (заглушка)");
            tokio::time::sleep(std::time::Duration::from_secs(3600)).await;
            Ok(())
        })
        .await;

        // Основной рендер-loop из yarilo-core
        let core = yarilo_core::YariloCore::new().await?;
        let core_arc = Arc::new(core);
        let core_for_x11 = core_arc.clone();

        self.spawn_restartable("X11 Server", move || {
            let core = core_for_x11.clone();
            async move { yarilo_x11::run_x11_server(core).await }
        })
        .await;

        // Основной рендер-loop теперь внутри Arc
        let mut core_shutdown = self.shutdown_tx.subscribe();
        tokio::spawn(async move {
            tokio::select! {
                res = core_arc.run() => {
                    if let Err(e) = res {
                        error!("YariloCore упал: {e:?}");
                    }
                }
                _ = core_shutdown.recv() => {
                    info!("YariloCore получил shutdown");
                }
            }
        });
        // Ждём Ctrl+C
        signal::ctrl_c().await?;
        info!("Получен Ctrl+C — шлём shutdown всем");
        let _ = self.shutdown_tx.send(());
        tokio::time::sleep(std::time::Duration::from_millis(500)).await; // даём задачам завершиться
        info!("AURORA SHUTDOWN COMPLETE");
        Ok(())
    }
}
