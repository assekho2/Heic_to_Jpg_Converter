@echo off
cd /d "%~dp0"
echo ============================================
echo   HEIC to JPG Converter
echo ============================================
echo.
echo Converting every .heic photo in the
echo "photos to convert" folder...
echo.
heic_converter_mt.exe "photos to convert"
echo.
echo Done. Your .jpg files are in the "output" folder.
echo.
pause
