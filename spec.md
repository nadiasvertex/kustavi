# Description

Kustavi is an image management application.

1. A front-end gui that allows a user to specify a folder that contains many images. The GUI shows a grid of the images in the folder so that the user can be sure that they have chosen the right folder.

2. It makes one pass, likely with opencv, to find blurry and poorly exposed images, and allows the user to mark them for deletion.

3. It makes another pass to find "junk" images like screenshots or things that aren't photographs. If possible it would be nice to use opencv for this, but if not then we should use an LLM with vision capability good enough to detect these kinds of junk images. If we use an LLM, I would prefer to embed the engine (likely llama.cpp, or the mlx equivalent on macos) instead of relying on ollama or similar. This will make deployment easier.

4. The next pass finds images which are similar. For example, a user might take several shots of the same scene in a row. They really only want to keep the best one from the sequence. We should be able to use opencv to determine similarity. Can we use opencv to determine "best" in terms of clarity and color balance and things like everyone's eyes open? In this pass, the app suggests which image the user should keep, but the user can override that and choose a different one. The duplicates are marked for deletion.

5. The final pass tries to find photos taken on the same "trip" or "vacation". We do this by finding pictures taken around the same time and, if possible, in the same location. For a particular "trip" this might be several days, and might involve a larger geographic area. For example, the user might drive to several places hundreds of kilometers apart of the the course of a few days. We should probably allow the user to provide some thresholds here.

6. When the user commits to their choices, we copy the images that were _not_ marked for deletion into a user selected folder and indicate that we are done.

The GUI is responsible for all user input, but the back end handles the actual image processing and classification. The back end should be launched by the front end, and they should communicate using gRPC, probably over a loopback port or domain socket.

If we need LLM support, the weights should be downloaded from huggingface the first time the user runs the program.

It needs to be able to run on a computer with only 16GB of RAM.

## Tech Stack Validation & Architecture

* Flutter + C++ via gRPC: This is an excellent choice. gRPC over a local loopback port (127.0.0.1:port) or Unix Domain Sockets (on macOS/Linux) ensures low-latency, strongly-typed IPC.
* Alternative to gRPC (Dart FFI): Instead of a separate process and gRPC, you could compile your C++ back-end as a dynamic library (.dll, .dylib, .so) and call it directly using Dart FFI (Foreign Function Interface).
* Pros of FFI: No network ports, no serialization overhead, simpler single-process deployment, easier to share memory pointers for raw images.
   * Pros of gRPC: Keeps the heavy C++ memory space isolated from the UI process, crash-resilient (if the backend crashes, the UI stays alive), easier to show progressive loading bars via gRPC streams. Stick to gRPC if you want isolated process safety.
* 16GB RAM Constraint: Totally viable. Local OpenCV operations take negligible RAM. An embedded vision LLM (like Moondream2 or LLaVA-Phi3 mini) quantized to Q4 via llama.cpp will only consume 2GB to 4GB of RAM, leaving plenty of headroom.

------------------------------

## Implementation Breakdown by Pass

## Pass 1: Folder Selection & Grid View

* Front-end: Use Flutter's file_picker package to get the directory path. Pass this path to the C++ back-end via gRPC.
* Optimizing the Grid: Do not load raw 4K images into Flutter's memory. Have the C++ back-end generate a cache folder of tiny, compressed JPEG thumbnails (e.g., 200px width) and stream the thumbnail paths back to the GUI. Flutter can then render them instantly using GridView.builder and Image.file.

## Pass 2: Blurry and Poorly Exposed Detection (OpenCV)

* Blur Detection: Compute the Laplacian variance of the image.

cv::Mat gray, laplacian;cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);cv::Laplacian(gray, laplacian, CV_64F);cv::Scalar mean, stddev;cv::meanStdDev(laplacian, mean, stddev);double variance = stddev_val * stddev_val; // Low variance = blurry

* Exposure Detection: Calculate a histogram of the grayscale image. If the pixels skew heavily toward 0 (underexposed) or 255 (overexposed), flag them. 

## Pass 3: Junk & Screenshot Detection (OpenCV vs. Local VLM)

* Can OpenCV do it? Yes, partially. Screenshots usually have massive blocks of perfectly flat, uniform colors and sharp text edges. You can run edge detection and count the ratio of unique colors or look for sharp horizontal/vertical lines (Hough Lines). However, it will fail on memes or digital art.
* The Better Approach (Embedded VLM): Use llama.cpp's native support for Vision-Language Models (VLM).
* Model choice: Moondream2 or LLaVA-v1.5-7B-quantized. Moondream is incredibly small (~2B parameters, <2GB RAM) and highly accurate at classifying images.
   * Prompt: "Is this image a real-life photograph? Answer with exactly 'yes' or 'no'."
   * HuggingFace Download: On first run, use Flutter or the C++ backend to fetch the .gguf file via an HTTP library directly into the user's local AppData/Application Support folder.

## Pass 4: Similarity Clustering & "Best Photo" Selection
 
* Finding Similar Images: Do not use pixel-by-pixel comparison. Use OpenCV Color Histograms (fastest) combined with Perceptual Hashing (pHash) or ORB feature matching. Group photos together if their visual hashes or histograms match within a tight threshold.
* Determining the "Best" Photo:
* Clarity: Use the Laplacian score from Pass 2 (higher is sharper).
   * Color Balance: Analyze the color histogram distribution. A balanced histogram with good contrast (well-distributed across the 0–255 spectrum) wins over washed-out profiles.
   * Eyes Open / Face Detection: Use OpenCV’s built-in Haar Cascades or DNN module (YuNet) to detect faces. You can crop the eyes and check their Laplacian variance to see if they are closed/blurry.
   * UI Logic: Present these in groups in Flutter. Pre-check the one with the highest composite score (Sharpness + Contrast + Eyes Open) and put a "Keep" badge on it, allowing the user to make a different selection to override.

## Pass 5: Trip / Vacation Clustering (Spatiotemporal)
 
* Metadata Extraction: Use a C++ library like exiv2 to extract the EXIF timestamp and GPS coordinates from the images during the initial indexing pass.
* Clustering Algorithm: Use DBSCAN (Density-Based Spatial Clustering of Applications with Noise).
* It groups points that are close to each other based on two parameters: Epsilon (maximum distance/time between points) and MinPoints.
   * Because trips span across time and space, customize the metric. If two photos are taken within 48 hours of each other, they belong to the same trip, even if the GPS coordinates jumped by 300 km (simulating driving or flying to a new spot).
   * Provide sliders in the Flutter GUI for: "Max days between stops" and "Max distance radius (km)". 

## Pass 6: Safe Commitment
 
* File Operations: When the user clicks "Process", the GUI sends a confirmation command. The backend loops through the approved image paths and uses std::filesystem::copy_file to move them to the target directory.
* Destructive Safety: Do not delete the original files automatically. Let the backend copy the clean files over, then display a success screen in Flutter saying: "Successfully copied X photos. You can safely delete the original folder manually."
