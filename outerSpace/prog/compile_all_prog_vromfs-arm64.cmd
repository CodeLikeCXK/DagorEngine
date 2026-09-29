call create_vfsroms-WOA.bat
if ERRORLEVEL 1 goto on_error

goto EOF

:on_error
echo "ERROR!!"
exit /b 1

:EOF
