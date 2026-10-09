#include <doctest/doctest.h>

#include "app/runtime.hpp"
#include "config/store.hpp"
#include "layout/book.hpp"
#include "media/image.hpp"
#include "media/imagestore.hpp"
#include "media/overlay.hpp"
#include "ops/api.hpp"
#include "ops/httpserver.hpp"
#include "ops/metrics.hpp"
#include "util/fetch.hpp"
#include "util/jsonutil.hpp"

#include "stb/stb_image_write.h"
#include <webp/encode.h>

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace mv;

namespace
{
// RGBA test pictures: left half red, right half blue, all opaque.
std::vector<std::uint8_t> halves(int width, int height)
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            auto* px = rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4u;
            px[0] = x < width / 2 ? 255 : 0;
            px[2] = x < width / 2 ? 0 : 255;
            px[3] = 255;
        }
    }
    return rgba;
}

void append(void* context, void* data, int size)
{
    static_cast<std::string*>(context)->append(static_cast<char const*>(data), static_cast<std::size_t>(size));
}

std::string png(int width, int height)
{
    auto const rgba = halves(width, height);
    std::string out;
    stbi_write_png_to_func(append, &out, width, height, 4, rgba.data(), width * 4);
    return out;
}

std::string jpeg(int width, int height)
{
    auto const rgba = halves(width, height);
    std::string out;
    stbi_write_jpg_to_func(append, &out, width, height, 4, rgba.data(), 90);
    return out;
}

std::string webp(int width, int height)
{
    auto const rgba = halves(width, height);
    std::uint8_t* encoded = nullptr;
    auto const size = WebPEncodeLosslessRGBA(rgba.data(), width, height, width * 4, &encoded);
    std::string out(reinterpret_cast<char const*>(encoded), size);
    WebPFree(encoded);
    return out;
}

// A GIF with the palette black, red, green, blue and one frame per entry of `frames` (palette
// indices of a frameWidth × frameHeight image at the top left), each `delayCs` hundredths of
// a second. The LZW data is the "uncompressed" form: a clear code before every two pixels
// keeps every code at 3 bits.
std::string gif(int width, int height, std::vector<std::vector<int>> const& frames, int frameWidth, int frameHeight, int delayCs)
{
    std::string g = "GIF89a";
    auto const le16 = [&](int v) {
        g += static_cast<char>(v & 255);
        g += static_cast<char>((v >> 8) & 255);
    };
    le16(width);
    le16(height);
    g += static_cast<char>(0x81);
    g += '\0';
    g += '\0';
    unsigned char const palette[12] = {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
    g.append(reinterpret_cast<char const*>(palette), sizeof(palette));
    for (auto const& pixels : frames)
    {
        g += "\x21\xF9\x04\x04";
        le16(delayCs);
        g += '\0';
        g += '\0';
        g += '\x2C';
        le16(0);
        le16(0);
        le16(frameWidth);
        le16(frameHeight);
        g += '\0';
        g += '\x02';
        std::vector<int> codes;
        for (std::size_t i = 0; i < pixels.size(); ++i)
        {
            if (i % 2 == 0)
            {
                codes.push_back(4);
            }
            codes.push_back(pixels[i]);
        }
        codes.push_back(5);
        std::string data;
        unsigned bits = 0;
        unsigned count = 0;
        for (int const code : codes)
        {
            bits |= static_cast<unsigned>(code) << count;
            count += 3;
            while (count >= 8)
            {
                data += static_cast<char>(bits & 255u);
                bits >>= 8;
                count -= 8;
            }
        }
        if (count > 0)
        {
            data += static_cast<char>(bits & 255u);
        }
        g += static_cast<char>(data.size());
        g += data;
        g += '\0';
    }
    g += '\x3B';
    return g;
}

std::uint32_t prgb(int r, int g, int b, int a)
{
    return static_cast<std::uint32_t>(a) << 24 | static_cast<std::uint32_t>(r) << 16 | static_cast<std::uint32_t>(g) << 8 | static_cast<std::uint32_t>(b);
}

// A local web server for the fetch tests; requests to 127.0.0.1 bypass any proxy.
struct PictureServer
{
    HttpServer http;
    std::string base;
    std::string picture = png(8, 4);

    PictureServer()
    {
        setenv("no_proxy", "127.0.0.1,localhost", 1);
        setenv("NO_PROXY", "127.0.0.1,localhost", 1);
        http.start(0, [this](HttpRequest const& request) {
            HttpResponse response;
            if (request.path == "/a.png" || request.path == "/big.png")
            {
                response.contentType = "image/png";
                response.body = request.path == "/a.png" ? picture : picture + std::string(kImageMaxBytes, '\0');
            }
            else if (request.path == "/page")
            {
                response.contentType = "text/html";
                response.body = "<html></html>";
            }
            else if (request.path == "/lie.png")
            {
                response.contentType = "image/png";
                response.body = gif(2, 2, {{1, 2, 3, 0}}, 2, 2, 10);
            }
            else if (request.path == "/slow.png")
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1200));
                response.contentType = "image/png";
                response.body = picture;
            }
            else
            {
                response.status = 404;
                response.body = "missing";
            }
            return response;
        });
        base = "http://127.0.0.1:" + std::to_string(http.port());
    }
};

// Asks until the store has a result (it loads on its own thread).
ImageStore::Picture waitFor(ImageStore& store, std::string const& url, std::string const& file, int width, int height)
{
    ImageStore::Picture picture;
    for (int i = 0; i < 300; ++i)
    {
        picture = store.get(url, file, width, height, ScaleMode::Fit);
        if (picture.image != nullptr || !picture.error.empty())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return picture;
}
} // namespace

TEST_CASE("pictures are PNG, JPEG, GIF, or WebP by their first bytes and by Content-Type")
{
    CHECK(sniffImage(png(4, 2)) == ImageType::Png);
    CHECK(sniffImage(jpeg(4, 2)) == ImageType::Jpeg);
    CHECK(sniffImage(gif(2, 2, {{1, 1, 1, 1}}, 2, 2, 0)) == ImageType::Gif);
    CHECK(sniffImage(webp(4, 2)) == ImageType::Webp);
    CHECK_FALSE(sniffImage("<html>").has_value());
    CHECK_FALSE(sniffImage("BM\x36\x00").has_value());
    CHECK(imageTypeOf("image/png") == ImageType::Png);
    CHECK(imageTypeOf("Image/JPEG; charset=binary") == ImageType::Jpeg);
    CHECK(imageTypeOf("image/webp") == ImageType::Webp);
    CHECK_FALSE(imageTypeOf("image/svg+xml").has_value());
    CHECK_FALSE(imageTypeOf("text/html").has_value());
    CHECK_FALSE(imageTypeOf("").has_value());
    CHECK(imageName("logo.png"));
    CHECK(imageName("Studio_2-bug.gif"));
    CHECK_FALSE(imageName(""));
    CHECK_FALSE(imageName(".hidden"));
    CHECK_FALSE(imageName("../config.json"));
    CHECK_FALSE(imageName("a/b.png"));
    CHECK_FALSE(imageName(std::string(101, 'a')));
}

TEST_CASE("pictures decode with their frames; the limits apply before decoding")
{
    for (auto const& bytes : {png(6, 4), jpeg(16, 8), webp(6, 4)})
    {
        DecodedImage image;
        REQUIRE_FALSE(decodeImage(bytes, image).has_value());
        CHECK(image.frames.size() == 1);
        CHECK(image.width > 0);
        auto const* first = image.frames[0].data();
        CHECK(first[0] > 200);
        CHECK(first[2] < 60);
        CHECK(first[3] == 255);
    }
    // An animated GIF: every frame, its time in ms (under 20 ms plays as 100 ms, as in browsers).
    DecodedImage animation;
    REQUIRE_FALSE(decodeImage(gif(2, 2, {{1, 1, 1, 1}, {2, 2, 2, 2}, {3, 3, 3, 3}}, 2, 2, 5), animation).has_value());
    CHECK(animation.type == ImageType::Gif);
    REQUIRE(animation.frames.size() == 3);
    CHECK(animation.delaysMs == std::vector<int>{50, 50, 50});
    CHECK(animation.frames[0][0] == 255);
    CHECK(animation.frames[1][1] == 255);
    CHECK(animation.frames[2][2] == 255);
    DecodedImage still;
    REQUIRE_FALSE(decodeImage(gif(2, 2, {{1, 2, 3, 0}}, 2, 2, 0), still).has_value());
    CHECK(still.delaysMs == std::vector<int>{100});

    DecodedImage refused;
    CHECK(decodeImage("<html></html>", refused).has_value());
    CHECK(decodeImage(png(4, 2) + std::string(kImageMaxBytes, '\0'), refused)->find("8 MiB") != std::string::npos);
    CHECK(decodeImage(png(4100, 2), refused)->find("4096") != std::string::npos);
    // A GIF whose screen is 4096 × 4096 with three frames (50 megapixels) is refused from its header.
    CHECK(decodeImage(gif(4096, 4096, {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}}, 2, 2, 10), refused)->find("megapixels") != std::string::npos);
    CHECK(decodeImage(png(8, 8).substr(0, 40), refused).has_value());
}

TEST_CASE("pictures scale into the tile: fit letterboxes, fill crops, premultiplied")
{
    DecodedImage image;
    REQUIRE_FALSE(decodeImage(png(4, 2), image).has_value());
    ScaledImage fit;
    REQUIRE_FALSE(scaleImage(image, 8, 8, ScaleMode::Fit, fit).has_value());
    CHECK(fit.x == 0);
    CHECK(fit.y == 2);
    CHECK(fit.width == 8);
    CHECK(fit.height == 4);
    CHECK(fit.frames[0][0] == prgb(255, 0, 0, 255));
    CHECK(fit.frames[0][7] == prgb(0, 0, 255, 255));
    ScaledImage fill;
    REQUIRE_FALSE(scaleImage(image, 4, 4, ScaleMode::Fill, fill).has_value());
    CHECK(fill.width == 4);
    CHECK(fill.height == 4);
    CHECK(fill.frames[0][0] == prgb(255, 0, 0, 255));
    CHECK(fill.frames[0][15] == prgb(0, 0, 255, 255));
    // Shrinking averages the area under each output pixel: white and black stripes turn grey.
    DecodedImage stripes;
    stripes.width = 4;
    stripes.height = 2;
    stripes.frames = {std::vector<std::uint8_t>(32, 0)};
    stripes.delaysMs = {100};
    for (int i = 0; i < 8; ++i)
    {
        auto const value = static_cast<std::uint8_t>(i % 2 == 0 ? 255 : 0);
        std::fill_n(stripes.frames[0].begin() + i * 4, 3, value);
        stripes.frames[0][static_cast<std::size_t>(i * 4 + 3)] = 255;
    }
    ScaledImage small;
    REQUIRE_FALSE(scaleImage(stripes, 2, 1, ScaleMode::Fit, small).has_value());
    REQUIRE(small.frames[0].size() == 2);
    for (auto const px : small.frames[0])
    {
        CHECK((px >> 24) == 255u);
        CHECK(std::abs(static_cast<int>((px >> 8) & 255u) - 128) <= 1);
    }
    // A transparent picture stays transparent, without dark fringes from its colour.
    DecodedImage clear;
    clear.width = 2;
    clear.height = 1;
    clear.frames = {{255, 255, 255, 0, 255, 0, 0, 255}};
    clear.delaysMs = {100};
    ScaledImage blend;
    REQUIRE_FALSE(scaleImage(clear, 2, 2, ScaleMode::Fill, blend).has_value());
    for (auto const px : blend.frames[0])
    {
        CHECK(((px >> 16) & 255u) <= (px >> 24));
        CHECK((px & 0xffffu) == 0);
    }
    // Animations play in a loop by their frame times.
    CHECK(imageFrameAt({100}, 5000) == 0);
    CHECK(imageFrameAt({100, 200, 100}, 0) == 0);
    CHECK(imageFrameAt({100, 200, 100}, 150) == 1);
    CHECK(imageFrameAt({100, 200, 100}, 350) == 2);
    CHECK(imageFrameAt({100, 200, 100}, 400) == 0);
}

TEST_CASE("an image tile draws its picture frame in the overlay")
{
    auto picture = std::make_shared<ScaledImage>();
    picture->x = 2;
    picture->y = 1;
    picture->width = 4;
    picture->height = 2;
    picture->frames = {std::vector<std::uint32_t>(8, prgb(255, 0, 0, 255)), std::vector<std::uint32_t>(8, prgb(64, 0, 0, 128))};
    picture->delaysMs = {100, 100};
    OverlayTile tile;
    tile.rect = {16, 0, 16, 8};
    tile.image = picture;
    Overlay overlay;
    overlay.resize(32, 8);
    renderOverlay(overlay, {tile});
    auto const at = [&](int x, int y) { return overlay.rgba.data() + static_cast<std::size_t>((y * overlay.width + x) * 4); };
    CHECK(at(19, 2)[0] == 255);
    CHECK(at(19, 2)[3] == 255);
    CHECK(at(17, 2)[3] == 0);
    CHECK(at(19, 4)[3] == 0);
    tile.imageFrame = 1;
    overlay.clear();
    renderOverlay(overlay, {tile});
    CHECK(at(19, 2)[3] == 128);
    CHECK(std::abs(at(19, 2)[0] - 128) <= 2);
}

TEST_CASE("pictures are fetched over http(s) only, with a size limit and a timeout")
{
    CHECK(httpUrl("https://example.org/logo.png"));
    CHECK(httpUrl("HTTP://10.0.0.1:8080/a.gif?x=1"));
    CHECK_FALSE(httpUrl("file:///etc/passwd"));
    CHECK_FALSE(httpUrl("ftp://example.org/a.png"));
    CHECK_FALSE(httpUrl("https:///a.png"));
    CHECK_FALSE(httpUrl("https://example.org/a b.png"));
    CHECK_FALSE(httpUrl("logo.png"));
    CHECK_FALSE(httpUrl("https://example.org/" + std::string(2100, 'a')));
    CHECK(fetchUrl("file:///etc/passwd", 1000, 100, 100).error.find("http") != std::string::npos);

    PictureServer server;
    auto const ok = fetchUrl(server.base + "/a.png", kImageMaxBytes, 1000, 3000);
    CHECK(ok.error.empty());
    CHECK(ok.status == 200);
    CHECK(ok.contentType == "image/png");
    CHECK(ok.body == server.picture);
    auto const missing = fetchUrl(server.base + "/nope", kImageMaxBytes, 1000, 3000);
    CHECK(missing.error.empty());
    CHECK(missing.status == 404);
    CHECK(fetchUrl(server.base + "/big.png", kImageMaxBytes, 1000, 3000).error == "the file is too large");
    auto const started = std::chrono::steady_clock::now();
    CHECK(fetchUrl(server.base + "/slow.png", kImageMaxBytes, 1000, 300).error == "timeout");
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1000));
    CHECK_FALSE(fetchUrl("http://127.0.0.1:1/a.png", kImageMaxBytes, 300, 300).error.empty());
}

TEST_CASE("the image store saves, lists, and scales pictures off the caller's thread")
{
    auto const dir = std::string("/tmp/mv-image-test");
    std::system(("rm -rf " + dir).c_str());
    PictureServer server;
    ImageStore store(dir);
    CHECK(store.list().empty());
    auto const saved = store.save("logo.png", "image/png", png(8, 4));
    REQUIRE(std::holds_alternative<ImageStore::Info>(saved));
    CHECK(std::get<ImageStore::Info>(saved).width == 8);
    CHECK(std::get<ImageStore::Info>(saved).frames == 1);
    CHECK(std::holds_alternative<std::string>(store.save("logo.png", "image/gif", png(8, 4))));
    CHECK(std::holds_alternative<std::string>(store.save("logo.png", "text/plain", png(8, 4))));
    CHECK(std::holds_alternative<std::string>(store.save("../logo.png", "image/png", png(8, 4))));
    CHECK(std::holds_alternative<std::string>(store.save("bad.png", "image/png", png(8, 4).substr(0, 30))));
    auto const listed = store.list();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].name == "logo.png");
    CHECK(listed[0].type == "image/png");
    CHECK(store.read("logo.png")->first == png(8, 4));

    // The first ask only queues the work.
    auto const first = store.get({}, "logo.png", 16, 16, ScaleMode::Fit);
    CHECK(first.image == nullptr);
    CHECK(first.error.empty());
    auto const stored = waitFor(store, {}, "logo.png", 16, 16);
    REQUIRE(stored.image != nullptr);
    CHECK(stored.image->width == 16);
    CHECK(stored.image->height == 8);
    CHECK(stored.image->y == 4);
    auto const fetched = waitFor(store, server.base + "/a.png", {}, 8, 4);
    REQUIRE(fetched.image != nullptr);
    CHECK(fetched.image->frames[0][0] == prgb(255, 0, 0, 255));
    CHECK(waitFor(store, server.base + "/page", {}, 8, 4).error == "not a picture (Content-Type text/html)");
    CHECK(waitFor(store, server.base + "/lie.png", {}, 8, 4).error == "the file is not image/png");
    CHECK(waitFor(store, server.base + "/nope.png", {}, 8, 4).error == "HTTP 404");
    CHECK(waitFor(store, {}, "missing.png", 8, 4).error == "no stored picture missing.png");
    CHECK(store.get({}, {}, 8, 4, ScaleMode::Fit).error == "no picture is set");
    // A slow URL holds up nobody: get() answers at once.
    auto const asked = std::chrono::steady_clock::now();
    CHECK(store.get(server.base + "/slow.png", {}, 8, 4, ScaleMode::Fit).image == nullptr);
    CHECK(std::chrono::steady_clock::now() - asked < std::chrono::milliseconds(50));

    CHECK(store.remove("logo.png"));
    CHECK_FALSE(store.remove("logo.png"));
    CHECK(store.list().empty());
}

TEST_CASE("the images API uploads, lists, serves, and deletes pictures")
{
    auto const dir = std::string("/tmp/mv-image-api-test");
    std::system(("rm -rf " + dir).c_str());
    std::map<std::string, std::string> env{{"NMOS_ENABLE", "false"}, {"MV_BACKEND", "cpu"}};
    ConfigStore config(env, std::nullopt);
    auto const cfg = config.effectiveConfig();
    LayoutBookStore layouts(cfg.maxInputs, cfg.activeLayout, "");
    RuntimeModel runtime(cfg);
    Metrics metrics;
    Api api(cfg, config, layouts, runtime, metrics);
    CHECK(api.handle(HttpRequest{"GET", "/api/v1/images", {}, {}, {}}).status == 404);
    ImageStore images(dir);
    api.setImages(images);
    auto const upload = [&](std::string const& name, std::string const& type, std::string const& body) {
        return api.handle(HttpRequest{"PUT", "/api/v1/images/" + name, {}, body, {{"content-type", type}}});
    };
    auto const created = upload("bug.gif", "image/gif", gif(2, 2, {{1, 1, 1, 1}, {2, 2, 2, 2}}, 2, 2, 10));
    CHECK(created.status == 201);
    CHECK(created.body.find("\"frames\":2") != std::string::npos);
    CHECK(upload("bug.gif", "image/png", gif(2, 2, {{1, 1, 1, 1}}, 2, 2, 10)).status == 400);
    CHECK(upload("x%2F..", "image/png", png(2, 2)).status == 400);
    std::string error;
    auto const list = json::parse(api.handle(HttpRequest{"GET", "/api/v1/images", {}, {}, {}}).body, error);
    REQUIRE(error.empty());
    CHECK(list.get("max_bytes").get<double>() == doctest::Approx(8 * 1024 * 1024));
    CHECK(list.get("images").get(0).get("name").get<std::string>() == "bug.gif");
    auto const served = api.handle(HttpRequest{"GET", "/api/v1/images/bug.gif", {}, {}, {}});
    CHECK(served.status == 200);
    CHECK(served.contentType == "image/gif");
    CHECK(api.handle(HttpRequest{"GET", "/api/v1/images/none.png", {}, {}, {}}).status == 404);

    // A picture a layout shows cannot be deleted.
    auto const wall = R"({"version":1,"name":"wall","tiles":[{"id":"i","content":"image","image_file":"bug.gif","rect":{"x":0,"y":0,"w":0.5,"h":0.5}}]})";
    CHECK(api.handle(HttpRequest{"PUT", "/api/v1/layouts/wall", {}, wall, {}}).status == 200);
    auto const blocked = api.handle(HttpRequest{"DELETE", "/api/v1/images/bug.gif", {}, {}, {}});
    CHECK(blocked.status == 409);
    CHECK(blocked.body.find("wall") != std::string::npos);
    CHECK(api.handle(HttpRequest{"DELETE", "/api/v1/layouts/wall", {}, {}, {}}).status == 200);
    CHECK(api.handle(HttpRequest{"DELETE", "/api/v1/images/bug.gif", {}, {}, {}}).status == 200);
    CHECK(api.handle(HttpRequest{"DELETE", "/api/v1/images/bug.gif", {}, {}, {}}).status == 404);
}
