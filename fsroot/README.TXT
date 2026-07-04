Welcome to the PumpkinOS filesystem!

This text is a real file stored on a FAT12 volume on the boot floppy.
The bootloader copies the filesystem image into memory, and the kernel's
FAT12 driver reads it - so 'ls' and 'cat' show actual on-disk files.

Try:
  ls
  cat poem.txt
  cat about.txt

To add your own files, drop them in the fsroot/ directory and rebuild.
