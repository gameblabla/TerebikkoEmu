# TerebikkoEmu by gameblabla

![User interface to TerebikkoEmu, playing Sailor S](https://raw.githubusercontent.com/gameblabla/TerebikkoEmu/refs/heads/main/screenshot.png)


TerebikkoEmu is a player/emulator for the Bandai Terebikko and its american counterpart, the Mattel See 'N Say Videophone.

It is capable of interpreting the 7-8khz tones encoded on the audio channels, and understanding the correct answer to the question asked.

At startup, when loading a new video file, it builds up a list of all of the tones in question in the video,
and constructs a playthrough accordingly.

Based on this information, it offers several game modes to make the games more interesting to play, then they would be otherwise.

Indeed, while the hardware is capable of interpreting such tones, it makes extremely poor use of it.

There are no consequences for answering wrong, the phone provides no feedback besides its internals (which the user doesn't know about).

33 titles were released for the Terebikko, as well as an additional 3 for the Videophone.

Personally, i must say, i was not expecting it to encode code onto the audio channels (the "buzzing" sounds that would play) :
Those were always suspicious to me and that has been confirmed.

This does unfortunately means that you need a good dump, particularly for the audio, for this emulator to play.
**Otherwise it will fail to pick up the questions entirely. I've already seen compressed dumps roaming around, that lost this information.**

While it can be reconstructed and i could build an internal database, it would go against the (spirit) of this device,
and its my opinion that these games should get redumped anyway.

# What do you need

- You simply need a high quality video / audio source

**Make sure it has the 7-8 khz audio tones fully intact, DO NOT FILTER THEM !**

Then you can drag and drop it into the window of the emulator meant for it.

If you have the equipment and your own VHS tapes, i strongly suggest you look into the vhs-decode project.
This will allow you to have your own, raw dump, that you can then pre-process.
You can also use your own AI upscaled videos, if that's what you prefer instead.

To my knowledge, there are no vhs-decode files for any of the Terebikko or See 'N Say VHS tapes.

- Optionally, subtitles, as these japanese titles will benefit from it.

These will need to be encoded into the mkv file as there's no separate file for providing them currently.

# How to play

The games on Terebikko are simple, the character on screen will instruct to either :
- Pick up the phone
- Answer a question correctly (out of 1-4, sometimes only 1-3 or even 1-2)

In some rare circumstances, like *See 'N Say Video Phone： Treasure Hunt*, 
the character may ask you to press one button on time.

For this game, an internal database had to be built for this specific edgecase as there is,
to my knowledge, no way of knowing, beyond guessing based on sound anchors, 
the exact timing it expects for the button to be pressed.

# Modes

There are 4 modes :
- Easy
- Hard
- Very Hard
- Mini-Game

Easy basically plays like the original hardware : you have infinite lives, no consequences for answering wrong.
Hard only gives you 3 lives and if you lose them all, you must start from scratch, additionally failling to pick up the phone in time also makes you lose one.
Very hard only has 1 life, the timings are much shorter, and you start from beginning again if you lose even once.

Mini-Game is special : it reuses clips from the game using the sound anchors, reusing the points for picking up phone and the questions.
This mode assumes that you played through the game at least once, as otherwise it will be trial and error.
It is a rhythm game, where you must pick up or answer as fast as possible, while music plays.

This mode can accept a custom music file. For games like Sailormoon, if none is provided, it will retrieve music from the video instead.
This may not be always possible (Mario on Terebikko has no long streaks of music in its VHS tape), for those a simple "music" is played instead.


# TODO 

- A native Windows / Linux build that leverages Qt, with gallery mode etc...
This is going to happen very soon, stay tuned.

- Play through more games
Unfortunately many do not have proper VHS dumps. Some use the tones very curiously as well.

- Help improve the database
Mostly for the other 2 See 'N Say games, and the (undumped) Japanese games.

- Add a multiplayer mode

