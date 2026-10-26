# Xbox Dev Mode Test Signing Certificate

`bloodborne_xbox-signing.pfx` is an intentionally public, passwordless, self-signed test
certificate (`CN=bloodborne_xbox`) used only for sideloading `.appx` packages in Xbox Series X|S
Developer Mode via Device Portal.

Keeping a consistent test publisher certificate across GitHub Actions builds lets newer `.appx`
builds upgrade an existing Dev Mode installation in place without wiping the player's save data,
shader cache, or prepared boot image in `LocalState`.

Do **not** commit private or production certificates to this directory (`xbox/.gitignore` blocks all
other `*.pfx` files).
