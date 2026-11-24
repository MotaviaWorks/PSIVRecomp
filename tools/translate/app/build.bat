@echo off
rem The translation editor as a Windows app:  build.bat WEBVIEW2_FOLDER OUT_FOLDER
rem WEBVIEW2_FOLDER holds Microsoft's web view files (from the Microsoft.Web.WebView2 package):
rem   Microsoft.Web.WebView2.Core.dll, Microsoft.Web.WebView2.WinForms.dll, WebView2Loader.dll (x64)
rem The compiler is the one that comes with Windows. The page (..\index.html) goes inside the program.
setlocal
if "%~2"=="" (echo usage: build.bat WEBVIEW2_FOLDER OUT_FOLDER & exit /b 1)
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%~2" mkdir "%~2"
for %%f in (Microsoft.Web.WebView2.Core.dll Microsoft.Web.WebView2.WinForms.dll WebView2Loader.dll) do copy /y "%~1\%%f" "%~2\" >nul || exit /b 1
"%CSC%" /nologo /target:winexe /platform:x64 /out:"%~2\TranslationEditor.exe" /win32icon:"%~dp0translate.ico" /resource:"%~dp0..\index.html",index.html /r:"%~2\Microsoft.Web.WebView2.Core.dll" /r:"%~2\Microsoft.Web.WebView2.WinForms.dll" /r:System.Web.Extensions.dll "%~dp0Translator.cs" || exit /b 1
echo built %~2\TranslationEditor.exe
