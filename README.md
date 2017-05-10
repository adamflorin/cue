# [cue]

`[cue]` is a Max external that lets you cue Max messages
to be dispatched at specified transport times.

## Using

Clone this repository into `Documents/Max 7/Packages` (or whatever the exact
path is on your Mac or Windows computer).

You should then be able to use the `[cue]` object and access its `.maxhelp`
patcher.

## Developing

**WARNING!** The Max SDK v7.1.0 posted on cycling74.com is several commits
behind `master` on GitHub! See https://github.com/Cycling74/max-sdk/issues/23.
The latest `[cue]` externals here were built against
https://github.com/Cycling74/max-sdk/commit/84044d393f62d80e8ef32b9adb6fbcb2d89a6149.

For Mac and PC:

- Download the [Max SDK](https://github.com/Cycling74/max-sdk) from GitHub
- Clone this repository into `source`
- Open Visual Studio or Xcode project file in `cue/source` and build
  (2x on Windows)
- Find build in SDK `externals` directory and copy to `externals` directory here

On Windows, the Visual Studio project was saved with VS Community 2017 on Windows 10.

On Mac, the Xcode project was saved with v8.3.2.
