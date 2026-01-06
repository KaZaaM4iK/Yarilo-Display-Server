use tracing_subscriber::{EnvFilter, fmt, prelude::*};
use yarilo_runner::AuroraSupervisor;

fn main() -> anyhow::Result<()> {
    tokio_uring::start(async {
        tracing_subscriber::registry()
            .with(fmt::layer())
            .with(EnvFilter::from_default_env())
            .init();

        tracing::info!("YARILO ПРОСНУЛСЯ — aurora-compositor v0.1");
        AuroraSupervisor::new().run().await
    })
}
