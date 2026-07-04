Welcome to the PumpkinOS filesystem!

This text is a real file stored on a FAT12 volume on the boot floppy, and
you are reading it straight off the disk. When you run 'cat', the kernel talks
to the floppy disk controller (the 8272 FDC) directly: it spins up the motor,
seeks to the right cylinder, sets up an ISA DMA transfer on channel 2, issues a
Read Data command, and waits for the drive's interrupt (IRQ 6). The sector
lands in a DMA buffer, and the FAT12 driver walks the file's cluster chain,
reading each cluster off the disk and printing it to the screen.

That is a genuine device driver reading genuine hardware - no cheating, no
RAM disk pre-loaded by the bootloader. The same code that works here in an
emulator should work on a real PC with a real floppy drive.

This particular file is deliberately a little long so that it spans more than
one cluster on the disk. Reading it therefore exercises the FAT chain: after
the first cluster, the driver looks up the next cluster number in the File
Allocation Table, seeks to it, reads it, and continues until it reaches the
end-of-chain marker. If you can read this final sentence in full, then
multi-cluster reads over the real floppy controller are working correctly.

Try these too:
  ls
  cat poem.txt
  cat about.txt

To add your own files, drop them into the fsroot/ directory and run 'make' -
the build reformats the FAT12 image and copies them in for you.
