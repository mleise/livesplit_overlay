## What Is the LiveSplit Overlay for Reshade?
It is a [ReShade](https://reshade.me/) addon for casual and serious speedrunners, that enables you to display the [LiveSplit](https://livesplit.org/) timer as an overlay inside the game itself.

- [When Do I Want to Use It?](#when-do-i-want-to-use-it-)
- [How Do I Install It?](#how-do-i-install-it-)
- [A Note on Fullscreen Modes](#a-note-on-fullscreen-modes)
- [Why use fullscreen over borderless window mode?](#why-use-fullscreen-over-borderless-window-mode-)
  * [🐌Lower Input lag](#--lower-input-lag)
  * [⛷️Variable Refresh Rate](#--variable-refresh-rate)
  * [☀️Power Savings](#--power-savings)
  * [💫Reduce Micro Stuttering](#--reduce-micro-stuttering)
- [Optimizing for Low Presentation Latency](#optimizing-for-low-presentation-latency)
- [Measuring Presentation Latency](#measuring-presentation-latency)

## When Do I Want to Use It?
When you are limited to a single monitor setup and you want to or have to run a certain title in fullscreen mode, you will have noticed that the LiveSplit window gets hidden by the game window even though LiveSplit is set to "stay on top". The solution thus far has been to use "borderless window" mode in the game settings if possible, or use external tools to force windowed mode.

## How Do I Install It?
Simply download the latest release on the right and depending on whether the game in question is 32-bit or 64-bit, drop either the `livesplit_overlay_x86.addon` or the `livesplit_overlay_x64.addon` into your game's binary directory next to ReShade. If you are unsure, just drop both files in there and ReShade will figure it out.

## A Note on Fullscreen Modes
Today there are many ways to present a game on screen that can be called "fullscreen". It has become more common now _not_ to use _exclusive_ fullscreen mode, but instead use something called "Windowed with Independent Flip", which gives us all the features of Exclusive Fullscreen Mode, paired with the ability to quickly tab between windows without the screen going blank for seconds. The various modes are explained by a Microsoft developer in this video: [DirectX 12: Presentation Modes In Windows 10](https://www.youtube.com/watch?v=E3wTajGZOsA)

## Why use fullscreen over borderless window mode?

### 🐌Lower Input lag
In general, when you play a game that is not fully covering the screen space or is overlapped by another window (in this case LiveSplit), you add 1 frame of input lag to your game experience. More accurately input lag increases by the reciprocal of your desktop refresh rate, which means 17 ms for 60 Hz and 7 ms for 144 Hz. This has to do with how the Desktop Window Manager (DWM) operates. A deep dive into the topic can be found here: [Present Latency, DWM and Waitable Swapchains](https://jackmin.home.blog/2018/12/14/swapchains-present-and-present-latency/)

### ⛷️Variable Refresh Rate
Variable refresh rates (GSYNC/FreeSync/Adaptive Sync) make it possible to synchronize your GPU with the monitor resulting in a smooth experience at high FPS and with low input lag while avoiding the screen tearing that previously came with turning VSYNC off. Variable refresh rate does not work with a borderless window that is overlapped by another.

### ☀️Power Savings
Typically you'll want to turn VSYNC off in-game with borderless window mode. This will not result in screen tearing, because the DWM is in control of the final image. At the same time the game can read your inputs at the fastest rate possible, which allows for the most consistent gameplay. On the downside, the DWM will go with the last frame that finished rendering at the last screen refresh. If you happen to have FPS 4 times higher than your monitor refresh rate, 75% of the frames are rendered but never displayed. Especially during the summer months, the extra power draw can quickly heat up your room and you could save quite a few kWh on your energy bill rendering only 1 frame per screen refresh!

### 💫Reduce Micro Stuttering
By extension of the above, if your frame rate is not several times higher than your monitor refresh rate, the frame pacing will be horrible. Imagine a situation where you have a 60 Hz monitor and 100 fps. If we write a 0 for every discarded frame and a one for every presented frame, it would look like this: 0101101011. The frames were rendered at a smooth 10 ms interval, but we are discarding 4 of them, so the 6 remaining frames represent 20, 20, 10, 20, 20, 10 milliseconds and are output to the monitor at a steady 16.6 ms pace. To the user this gives the appearance of the game intermittedly speeding up and slowing down or micro stuttering.

## Optimizing for Low Presentation Latency
This section is aimed users of monitors without variable refresh rate that want to get the most out of it. After reading all of the above, it might sound like once in fullscreen mode, you just turn on VSYNC and you get the best experience. Not quite! Older games are often done rendering within a few milliseconds and spend most time waiting on VSYNC to present the frame. During that time the game cannot react to your input.

Here is an example assuming 16 ms frame times: The game has rendered a frame which took 4 ms. Now it is waiting for the remaining 12 ms on VSYNC to present the frame. 10 ms into the frame you click your mouse to fire a gun, but the game is already done with the frame and can no longer process your input. The shot will have to wait to the next frame and we'll see it's effect after 22 ms.

If we could move the 12 ms waiting time to the start of the frame, and start rendering just in time to finish for the VSYNC signal, we could press the mouse button after 10 ms and the game would still be able to react to it. Within only 6 ms we would see the effect of the shot on screen!

This is possible, and both AMD and NVIDIA have special low lag options in their drivers that do just that. There is a downside to that though: If by chance - thanks to us delaying the rendering of the frame - it doesn't get finished in time for the VSYNC, we have to wait an entire frame for it to show up. This could do more harm to the frame pacing than it does good.

A different approach is to turn VSYNC off in-game and have an external program handle it "in software". What that means is that a piece of software delays the game's rendering as above, but there is no hardware VSYNC, so if we miss by a bit we don't have to wait for the next one. Instead we just present the frame a little late, resulting in a bit of screen tearing at the top of the monitor, but the frame pacing stays intact.

For that you can use RivaTuner Statistics Server (RTSS), which typically comes bundled with the tuning app MSI Afterburner and is used to present sensor readings in-game. It also comes with a great frame limiter though. It is found in "Setup"->"General"->"Enable framerate limiter". The option "Enable passive waiting" is basically telling the operating system "Call me when it's time to start rendering!" instead of going "Are we there yet? Are we there yet?" constantly. The latter is more precise, but it will clog one CPU core 100% all of the time. If you are dealing with a game that has some background processing going on and needs every core, you might want to skip on it. Lastly you should close the options, click on "Add" to add your game executable and set "Scanline sync" to a low negative value. This has the effect of RTSS trying to delay the game just enough to be able to present the next frame at that many scanlines (or pixel rows) *before* the top of the screen. Due to how display timings work there are always a couple of blank scanlines in this "negative" area that you can use to hide the screen tearing in. The farther into the negatives you go, the less likely it is for small hickups to result in visible screen tearing, but eventually you'll come back in at the bottom of the screen. For games with inconsistent frame times you wont be able to completely hide screen tearing, but you'll get the best possible frame pacing on your fixed refresh rate monitor!

An in depth article that has more details and options can be found here: [How to use RTSS's Scanline Sync](https://www.resetera.com/threads/guide-how-to-use-rtsss-scanline-sync-to-reduce-stuttering-screen-tearing-and-input-lag-on-pc-alternative-to-vsync-g-sync-and-freesync.138764/)

## Measuring Presentation Latency
With all that said you might wonder how you can personally verify that you are indeed improving your Presentation Latency and what I mean by that is the time it takes from the game telling the 3D API that it has sent all commands for the frame to the GPU to the time the rendered image is delivered to the screen.

For this there are several tools and I just used [PresentMon](https://github.com/GameTechDev/PresentMon) directly, but the web site also lists a couple tools that come with a graphical UI.

For some example data I used Alien: Isolation, set my monitor refresh rate to 60 Hz and ran three different scenarios, looking at the Presentation Latency:

1. Borderless window mode, VSYNC off, LiveSplit window on top. This is the way it is normaly run.
2. Fullscreen mode, VSYNC on, LiveSplit shown in-game via this add-on.
3. Fullscreen mode, RivaTuner software scanline sync, LiveSplit again via add-on.

In the first case, the game ran at 210 fps and due to the DWM doing its thing, the Presentation Latency fluctuated between 19 ms and 23 ms. This is equal to one refresh interval + a fraction of the frame time + a bit.

The second case was interesting, becasue players who tried Alien: Isolation at both 60 Hz and higher refresh rates noticed that it felt sluggish at 60 Hz. It is as if the character is drunk or movement feels weighty like a tank turret. And sure enough the numbers reflect that: On average we get 57.7 ms of Presentation Latency in this mode. This is obviously a bug of sorts as that is about 3.5 frames of waiting till your character reacts.

The third test is my recommendation. With the RTSS software handling the presentation, we get an average Presentation Latency of just 0.3 ms, very consistent frame pacing and reduced heat output!
