pascal-dxr-tier-1.1  v{VERSION}
================================================================

Makes NVIDIA Pascal (GTX 10-series) and GTX 16-series cards report DXR
Tier 1.1, by rewriting RayQuery shaders into DXR 1.0 at runtime.

It does NOT implement ray tracing. Your GPU has been able to trace rays
since driver 425.31 in 2019; what it lacked was the Tier 1.1 API. This
translates the API and lets the driver do every ray, which is why results
match an RTX card rather than approximating one.

Windows x64 only.


QUICK START
----------------------------------------------------------------

Run dxr-tier-11-setup.bat, pick the game or editor .exe, click Install.

Or by hand: copy d3d12.dll into the folder containing the .exe. That is
the whole installation. Tier 1.1 is on by default.

For Unreal that folder is usually:
    Unreal Editor    Engine\Binaries\Win64\UnrealEditor.exe
    A packaged game  <Project>\Binaries\Win64\<Project>.exe

It must sit beside the .exe itself, not beside a launcher and not in the
game's root folder.


YOU ALSO NEED TWO FILES FROM MICROSOFT
----------------------------------------------------------------

Ray tracing shaders cannot be rewritten without them:

    dxcompiler.dll
    dxil.dll

Get them here, from the newest release, inside bin\x64\ :

    https://github.com/microsoft/DirectXShaderCompiler/releases

Copy both into the same folder as d3d12.dll. Take both from the SAME
release; a mismatched pair fails to sign.

They are not included here because they belong to Microsoft, and this
project redistributes nothing that is not its own.

Without them nothing breaks. The shim reports the real tier, stands
aside completely, and your game runs exactly as it did before, just
without ray tracing. The log says so.


SETTINGS
----------------------------------------------------------------

Optional. Copy dxr-tier-11.example.ini next to d3d12.dll, rename it to
dxr-tier-11.ini, and edit it. A file is used rather than environment
variables because a game launched from Steam or the Epic launcher never
sees a variable you set in a console.

    tier11 = 0     report the real tier, translate nothing
    nowrap = 1     diagnostic: leave the DLL loaded but switch the whole
                   layer off, including Tier 1.1


UNINSTALL
----------------------------------------------------------------

Delete d3d12.dll from the game folder. That restores the original
behaviour exactly.

Nothing is installed anywhere else. No registry keys, no services, and
no files outside that folder and one log in %TEMP%.


WHEN SOMETHING GOES WRONG
----------------------------------------------------------------

Read the log first, always:

    %TEMP%\dxr-tier-11-proxy.log

Its first line names the version, so a bug report identifies itself. The
setup tool has a "Show refusals only" button that filters it down to
what could not be translated and why.

Some shaders are refused on purpose. DXR 1.0 has no equivalent for parts
of Tier 1.1, and a refused shader is passed through untouched so the
driver reports its own error rather than this producing a wrong picture.
The full list is in the README on the project page.

If your antivirus objects: this is an unsigned DLL that gets copied into
game folders, which is a shape that trips heuristics. The source is
public and every release is built from a tagged commit.


NOT AFFILIATED with NVIDIA or Microsoft, and redistributing no code
belonging to either.

    https://github.com/mohamedfawzyps/pascal-dxr-tier-1.1
