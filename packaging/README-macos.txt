HEIC to JPG Converter (macOS)
=============================

This tool converts Apple HEIC photos (.heic) into standard JPG images.


HOW TO USE
----------

1. Put the .heic photos you want to convert into the "photos to convert"
   folder. It is in this same folder, right next to the heic_converter_mt
   program.

2. Open the Terminal app in this folder (in Finder, right-click this folder
   and choose Services > "New Terminal at Folder", or drag the folder onto
   the Terminal icon).

3. The first time only, allow this unsigned program to run by pasting this
   line and pressing Enter:

       xattr -d com.apple.quarantine ./heic_converter_mt

4. Run the converter:

       ./heic_converter_mt "photos to convert"

5. The converted .jpg images appear in a new "output" folder created here.
   That's it!


OPTIONS (optional)
------------------
You can add these after the command in step 4:

  -q <1-100>   JPEG quality (default: 90)
  -t <n>       number of worker threads (default: all CPU cores)
  -o <folder>  output folder (default: output)
  --help       show full help

Example:

       ./heic_converter_mt "photos to convert" -q 85
