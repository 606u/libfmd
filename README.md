# libfmd

`libfmd` is a simple file metadata scanning library with (partial support) for:

  * selected FLAC, MP3, MP4 media files,
  * selected TIFF and JPEG picture files,
  * archive files, supported by `libarchive`.

## Dependencies

Only `libarchive` at this time.

## Build

    make

## Sample output

    # fmdscan -a song.m4a movie.m4v photo.nef
    song.m4a (song.m4a)
      filetype: 'audio'
      mimetype: 'audio/mp4'
      dev -6320774731036347803, ino 20826, links 1
      size 121257303, blksize 131072, blocks 237217
      atime: 2019-10-06 21:59:15
      mtime: 2019-10-06 21:59:22
      ctime: 2019-10-06 21:59:22
      uid 0, gid 20, mode 0100644
    	artist: 'Ólafur Arnalds'
    	creator: 'X Lossless Decoder 20191004, QuickTime 7.7.3'
    	trackno: 1
    	album: 're:member'
    	performer: 'Ólafur Arnalds'
    	title: 're:member'
    	duration: 364.648000
    movie.m4v (movie.m4v)
      filetype: 'video'
      mimetype: 'video/mp4'
      dev -6320774731036347803, ino 21080, links 1
      size 3091796851, blksize 131072, blocks 6042865
      atime: 2020-02-17 17:57:23
      mtime: 2020-02-17 18:01:06
      ctime: 2020-02-17 18:01:48
      uid 0, gid 20, mode 0100644
    	description: '“Joker” centers around the iconic arch-nemesis and is an original, standalone story not seen before on the big screen. The exploration of Arthur Fleck (Joaquin Phoenix), a man disregarded by society, is not only a gritty character study, but also a broader cautionary tale.'
    	performer: 'Todd Phillips'
    	title: 'Joker'
    	duration: 7309.120000
    photo.nef (photo.nef)
      filetype: 'raster'
      mimetype: 'image/tiff'
      dev -6320774731036347803, ino 19597, links 1
      size 14599017, blksize 131072, blocks 28705
      atime: 2019-01-31 19:49:41
      mtime: 2019-01-31 19:49:42
      ctime: 2019-01-31 19:49:42
      uid 0, gid 0, mode 0100600
    	focal_length35: 52.000000
    	focal_length: 35.000000
    	iso_speed: 200
    	fnumber: 5.000000
    	exposure_time: 1/60
    	artist: 'user@acme.com                       '
    	creator: 'Ver.1.01 '
    	creator: 'NIKON D300S'
    	creator: 'NIKON CORPORATION'
    	bits_per_sample: 24
    	num_channels: 3
    	frame_height: 120
    	frame_width: 160

## Documentation

Not yet; read `fmd.h` and `fmdscan.c`.
