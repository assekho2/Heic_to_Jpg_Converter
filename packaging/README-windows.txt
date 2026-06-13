HEIC to JPG Converter (Windows)
===============================

This tool converts Apple HEIC photos (.heic) into standard JPG images.


HOW TO USE
----------

1. Put the .heic photos you want to convert into the "photos to convert"
   folder. It is in this same folder, right next to heic_converter_mt.exe.

2. Open this folder in File Explorer, click the address bar, type:

       cmd

   and press Enter. A black terminal window opens in this folder.

3. In that terminal, type the following and press Enter:

       heic_converter_mt.exe "photos to convert"

4. The converted .jpg images appear in a new "output" folder created here.
   That's it!


NOTE: "Windows protected your PC"
---------------------------------
Windows SmartScreen may warn about this program because it is not signed.
It is safe to run: click "More info", then "Run anyway".


OPTIONS (optional)
------------------
You can add these after the command in step 3:

  -q <1-100>   JPEG quality (default: 90)
  -t <n>       number of worker threads (default: all CPU cores)
  -o <folder>  output folder (default: output)
  --help       show full help

Example:

       heic_converter_mt.exe "photos to convert" -q 85
