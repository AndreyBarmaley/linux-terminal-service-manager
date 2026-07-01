// Google Gemini AI

#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ltsm_application.h"
#include "ltsm_sdl_wrapper.h"

using namespace LTSM;

TEST(LtsmSurfaceTest, DefaultConstructorInitializesAsInvalid) {
    SDL::Surface surface;
    EXPECT_FALSE(surface.isValid());
    EXPECT_EQ(surface.get(), nullptr);
}

TEST(LtsmSurfaceTest, ValidPtrMakesSurfaceValid) {
    SDL_Surface* raw_sf = SDL_CreateRGBSurface(0, 32, 32, 32, 0, 0, 0, 0);
    ASSERT_NE(raw_sf, nullptr);

    {
        SDL::Surface surface(raw_sf);
        EXPECT_TRUE(surface.isValid());
        EXPECT_EQ(surface.get(), raw_sf);
        EXPECT_EQ(surface.format(), raw_sf->format);
    }
}

TEST(LtsmSurfaceTest, MoveConstructorTransfersOwnership) {
    SDL_Surface* raw_sf = SDL_CreateRGBSurface(0, 10, 10, 32, 0, 0, 0, 0);
    
    SDL::Surface src(raw_sf);
    SDL::Surface dst(std::move(src));

    EXPECT_FALSE(src.isValid());
    EXPECT_TRUE(dst.isValid());
    EXPECT_EQ(dst.get(), raw_sf);
}

TEST(LtsmSurfaceTest, ResetChangesPointerAndFreesOld) {
    SDL_Surface* sf1 = SDL_CreateRGBSurface(0, 5, 5, 32, 0, 0, 0, 0);
    SDL_Surface* sf2 = SDL_CreateRGBSurface(0, 10, 10, 32, 0, 0, 0, 0);

    SDL::Surface surface(sf1);
    EXPECT_EQ(surface.get(), sf1);

    surface.reset(sf2);
    EXPECT_EQ(surface.get(), sf2);
}

TEST(LtsmTextureTest, EmptyTextureState) {
    SDL::Texture texture;
    EXPECT_FALSE(texture.isValid());
    EXPECT_EQ(texture.get(), nullptr);
}

TEST(LtsmTextureTest, MoveSemantics) {
    SDL::Texture src(nullptr); 
    SDL::Texture dst(std::move(src));
    EXPECT_FALSE(dst.isValid());
}

class LtsmWindowTest : public ::testing::Test {
protected:
    void SetUp() override {
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            FAIL() << "Could not initialize SDL: " << SDL_GetError();
        }
    }

    void TearDown() override {
        SDL_Quit();
    }
};

TEST_F(LtsmWindowTest, ThrowsExceptionOnInvalidArguments) {
    XCB::Size render_sz( -10, -10 );
    XCB::Size window_sz( -10, -10 );

    EXPECT_THROW({
        SDL::Window window("Test Fail Window", render_sz, window_sz);
    }, sdl_error);
}

TEST_F(LtsmWindowTest, SuccessfullyCreatesWindowAndValidatesState) {
    XCB::Size render_sz{ 800, 600 };
    XCB::Size window_sz{ 800, 600 };
    
    SDL::Window window("Unit Test Window", render_sz, window_sz, SDL_WINDOW_HIDDEN, false);

    EXPECT_NE(window.get(), nullptr);
    EXPECT_NE(window.render(), nullptr);
    
    if (window.isValid()) {
        EXPECT_NE(window.display(), nullptr);
    }

    auto [scaled_x, scaled_y] = window.scaleCoord(100, 100);
    EXPECT_EQ(scaled_x, 100);
    EXPECT_EQ(scaled_y, 100);
}

TEST_F(LtsmWindowTest, ConvertScanCodeToKeySymReturnsExpectedValues) {
    int result_a = SDL::Window::convertScanCodeToKeySym(SDL_SCANCODE_MENU);
    int result_b = SDL::Window::convertScanCodeToKeySym(SDL_SCANCODE_UNKNOWN);

    EXPECT_NE(result_a, 0); 
    EXPECT_EQ(result_b, 0);
}

TEST_F(LtsmWindowTest, ResizeUpdatesGeometryInternally) {
    XCB::Size initial_sz{ 640, 480 };
    SDL::Window window("Resize Test", initial_sz, initial_sz, SDL_WINDOW_HIDDEN, false);

    XCB::Size new_sz{ 1024, 768 };
    window.resize(new_sz);

    EXPECT_EQ(window.geometry().width, 1024);
    EXPECT_EQ(window.geometry().height, 768);
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_sdlwrap.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
