import worlds.LauncherComponents as LauncherComponents

from .world import RoboRumbleWorld


def launch_client(*args) -> None:
    from .client import main
    LauncherComponents.launch(main, name="RoboRumbleClient", args=args)


LauncherComponents.components.append(
    LauncherComponents.Component(
        "RoboRumble Client",
        func=launch_client,
        component_type=LauncherComponents.Type.CLIENT,
        game_name="RoboRumble",
        supports_uri=True,
    )
)

__all__ = ["RoboRumbleWorld"]
