OBS Studio <https://obsproject.com>
===================================

.. image:: https://badges.crowdin.net/obs-studio/localized.svg
   :alt: OBS Studio Translation Project Progress
   :target: https://crowdin.com/project/obs-studio

.. image:: https://img.shields.io/discord/348973006581923840.svg?label=&logo=discord&logoColor=ffffff&color=7389D8&labelColor=6A7EC2
   :alt: OBS Studio Discord Server
   :target: https://obsproject.com/discord

About this fork
---------------

This is a personal fork of `OBS Studio <https://github.com/obsproject/obs-studio>`_,
based on version 30.1.2, with a few performance and bug-fix changes:

- Fix for the VRAM/X11 window leak when toggling the preview (EGL swapchain).
- Optional per-source CPU cost profiler (``OBS_SOURCE_PROFILER=1``).
- XSHM screen capture with GPU copy (``Copy method``: Automatic/GPU/SHM).
- Fewer X server round trips in the Window Capture (Xcomposite) source.
- Preview refresh rate setting (General > Preview).

CI is intentionally minimal: pushes and pull requests run the formatting
checks (clang-format, cmake-format, Qt XML), and pushing a tag builds ``.deb`` packages for the
Ubuntu LTS releases (22.04 and 24.04) and for Debian 12 and 13 (usable on Devuan Daedalus and Excalibur) and attaches them to a GitHub
release (see ``.github/workflows/release-deb.yaml``). Tags should look like
``30.1.3`` or ``v30.1.3``, since the version is taken from the tag name. The
other upstream workflows were removed from this fork.

What is OBS Studio?
-------------------

OBS Studio is software designed for capturing, compositing, encoding,
recording, and streaming video content, efficiently.

It's distributed under the GNU General Public License v2 (or any later
version) - see the accompanying COPYING file for more details.

Quick Links
-----------

- Website: https://obsproject.com

- Help/Documentation/Guides: https://github.com/obsproject/obs-studio/wiki

- Forums: https://obsproject.com/forum/

- Build Instructions: https://github.com/obsproject/obs-studio/wiki/Install-Instructions

- Developer/API Documentation: https://obsproject.com/docs

- Donating/backing/sponsoring: https://obsproject.com/contribute

- Bug Tracker: https://github.com/obsproject/obs-studio/issues

Contributing
------------

- If you would like to help fund or sponsor the project, you can do so
  via `Patreon <https://www.patreon.com/obsproject>`_, `OpenCollective
  <https://opencollective.com/obsproject>`_, or `PayPal
  <https://www.paypal.me/obsproject>`_.  See our `contribute page
  <https://obsproject.com/contribute>`_ for more information.

- If you wish to contribute code to the project, please make sure to
  read the coding and commit guidelines:
  https://github.com/obsproject/obs-studio/blob/master/CONTRIBUTING.rst

- Developer/API documentation can be found here:
  https://obsproject.com/docs

- If you wish to contribute translations, do not submit pull requests.
  Instead, please use Crowdin.  For more information read this page:
  https://obsproject.com/wiki/How-To-Contribute-Translations-For-OBS

- Other ways to contribute are by helping people out with support on
  our forums or in our community chat.  Please limit support to topics
  you fully understand -- bad advice is worse than no advice.  When it
  comes to something that you don't fully know or understand, please
  defer to the official help or official channels.
