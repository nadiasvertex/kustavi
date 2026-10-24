I looked at the specs and the current pipeline: scan, quality, junk, similar, video, trips, and a copy-only commit. Three features would help most with large offline collections, which tend to be messy, built from many sources, and full of overlaps. They are cross-source duplicate detection, date and location repair, and keeping related files together. The rest of the list is grouped by how much work each item would take.

Recommended first

1. Exact and near-exact duplicates across sources. The similar pass finds burst shots. Old collections have a different problem: the same photo exists several times, from phone backups, re-compressed messenger copies, edited exports, and resized versions. A content hash plus a resolution-independent perceptual hash would catch these. When duplicates differ in quality, the app should keep the copy with the highest resolution or the original EXIF.

Moderate effort

5. Commit verification and a report. The app never touches the source, so the user's next step is usually to delete the source themselves. Checking each copy's checksum after commit and writing a manifest of what was copied, what was excluded, and why would make that step safe. Exporting the excluded list as a folder of symlinks or a CSV would also help.
7. Corrupt-file detection. Flag truncated JPEGs, zero-byte files and undecodable images during the scan. These are common in old backups. Skip this if the scan already does it.
8. Highlights per trip. Use the existing keeper scores to suggest the best handful of photos per trip as a short "best of" folder.

Larger features

9. People grouping. YuNet is already in use. Adding OpenCV's SFace embeddings would allow clustering faces by person, entirely offline, and adding a person filter to the grid.
10. Text search and tags. Qwen2.5-VL is already downloaded for the junk pass, so it could also produce short tags or captions ("beach", "birthday cake", "document") for search. The cost is runtime: on a 16 GB machine this would be a slow, optional, resumable pass.
11. Interoperability through XMP. Write ratings, reject flags, trip names and people tags as XMP sidecars, so Lightroom, digiKam or darktable can read the curation work.
12. Video-specific junk. Flag very short accidental clips, such as pocket recordings and black frames, and near-duplicate videos.
