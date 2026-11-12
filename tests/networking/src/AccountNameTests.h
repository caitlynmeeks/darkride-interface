//
//  AccountNameTests.h
//  tests/networking/src
//
//  The names the door takes from a connect request and how it asks the metaverse about one (AccountName.h): no
//  network, no server.
//
//  Created by servers on 2026-11-12.
//  Copyright 2026 Darkride.
//
//  Distributed under the MIT License.
//  See the accompanying file LICENSE-MIT or https://opensource.org/licenses/MIT
//

#ifndef hifi_AccountNameTests_h
#define hifi_AccountNameTests_h

#include <QtTest/QtTest>

class AccountNameTests : public QObject {
    Q_OBJECT
private slots:
    void notAccountNames_data();
    void notAccountNames();
    void accountNames_data();
    void accountNames();
    void anAnswerWithoutItsNameIsDropped();
};

#endif // hifi_AccountNameTests_h
